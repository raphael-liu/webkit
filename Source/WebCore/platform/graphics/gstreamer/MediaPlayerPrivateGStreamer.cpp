/*
 * Copyright (C) 2007, 2009 Apple Inc.  All rights reserved.
 * Copyright (C) 2007 Collabora Ltd.  All rights reserved.
 * Copyright (C) 2007 Alp Toker <alp@atoker.com>
 * Copyright (C) 2009 Gustavo Noronha Silva <gns@gnome.org>
 * Copyright (C) 2014 Cable Television Laboratories, Inc.
 * Copyright (C) 2009, 2019 Igalia S.L
 * Copyright (C) 2015, 2019 Metrological Group B.V.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public License
 * aint with this library; see the file COPYING.LIB.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#include "config.h"
#include "MediaPlayerPrivateGStreamer.h"

#if ENABLE(VIDEO) && USE(GSTREAMER)

#include "GraphicsContext.h"
#include "GStreamerCommon.h"
#include "GStreamerRegistryScanner.h"
#include "HTTPHeaderNames.h"
#include "ImageGStreamer.h"
#include "ImageOrientation.h"
#include "IntRect.h"
#include "Logging.h"
#include "MediaPlayer.h"
#include "MediaPlayerRequestInstallMissingPluginsCallback.h"
#include "MIMETypeRegistry.h"
#include "NotImplemented.h"
#include "SecurityOrigin.h"
#include "TimeRanges.h"
#include "VideoSinkGStreamer.h"
#include "WebKitWebSourceGStreamer.h"

#if ENABLE(VIDEO_TRACK)
#include "AudioTrackPrivateGStreamer.h"
#include "InbandMetadataTextTrackPrivateGStreamer.h"
#include "InbandTextTrackPrivateGStreamer.h"
#include "TextCombinerGStreamer.h"
#include "TextSinkGStreamer.h"
#include "VideoTrackPrivateGStreamer.h"
#endif // ENABLE(VIDEO_TRACK)

#if ENABLE(MEDIA_STREAM)
#include "GStreamerMediaStreamSource.h"
#endif

#if ENABLE(MEDIA_SOURCE)
#include "MediaSource.h"
#include "WebKitMediaSourceGStreamer.h"
#endif

#if ENABLE(ENCRYPTED_MEDIA)
#include "CDMInstance.h"
#include "GStreamerEMEUtilities.h"
#include "SharedBuffer.h"
#include "WebKitCommonEncryptionDecryptorGStreamer.h"
#endif

#if ENABLE(WEB_AUDIO)
#include "AudioSourceProviderGStreamer.h"
#endif

#include <glib.h>
#include <gst/audio/streamvolume.h>
#include <gst/gst.h>
#include <gst/pbutils/missing-plugins.h>
#include <gst/video/gstvideometa.h>
#include <limits>
#include <wtf/FileSystem.h>
#include <wtf/glib/GLibUtilities.h>
#include <wtf/glib/GUniquePtr.h>
#include <wtf/glib/RunLoopSourcePriority.h>
#include <wtf/MathExtras.h>
#include <wtf/MediaTime.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/StringPrintStream.h>
#include <wtf/text/AtomString.h>
#include <wtf/text/CString.h>
#include <wtf/text/StringConcatenateNumbers.h>
#include <wtf/URL.h>
#include <wtf/WallTime.h>

#if ENABLE(VIDEO_TRACK) && USE(GSTREAMER_MPEGTS)
#define GST_USE_UNSTABLE_API
#include <gst/mpegts/mpegts.h>
#undef GST_USE_UNSTABLE_API
#endif // ENABLE(VIDEO_TRACK) && USE(GSTREAMER_MPEGTS)

#if ENABLE(VIDEO_TRACK)
#define CREATE_TRACK(type, Type) G_STMT_START {                         \
        m_has##Type = true;                                             \
        if (!useMediaSource) {                                          \
            RefPtr<Type##TrackPrivateGStreamer> track = Type##TrackPrivateGStreamer::create(makeWeakPtr(*this), i, stream); \
            m_##type##Tracks.add(track->id(), track);                   \
            m_player->add##Type##Track(*track);                         \
            if (gst_stream_get_stream_flags(stream.get()) & GST_STREAM_FLAG_SELECT) \
                m_current##Type##StreamId = String(gst_stream_get_stream_id(stream.get())); \
        }                                                               \
    } G_STMT_END

#define CLEAR_TRACKS(tracks, method) \
    for (auto& track : tracks.values())\
        method(*track);\
    tracks.clear();
#else
#define CREATE_TRACK(type, Type) G_STMT_START { \
        m_has##Type## = true;                   \
    } G_STMT_END
#endif // ENABLE(VIDEO_TRACK)

#if USE(GSTREAMER_GL)
#define TEXTURE_COPIER_COLOR_CONVERT_FLAG VideoTextureCopierGStreamer::ColorConversion::NoConvert
#define GST_GL_CAPS_FORMAT "{ RGBx, RGBA, I420, Y444, YV12, Y41B, Y42B, NV12, NV21, VUYA }"

#include <gst/app/gstappsink.h>

#include "GLContext.h"
#if USE(GLX)
#include "GLContextGLX.h"
#include <gst/gl/x11/gstgldisplay_x11.h>
#endif

#if USE(EGL)
#include "GLContextEGL.h"
#include <gst/gl/egl/gstgldisplay_egl.h>
#endif

#if PLATFORM(X11)
#include "PlatformDisplayX11.h"
#endif

#if PLATFORM(WAYLAND)
#include "PlatformDisplayWayland.h"
#endif

#if USE(WPE_RENDERER)
#include "PlatformDisplayLibWPE.h"
#endif

// gstglapi.h may include eglplatform.h and it includes X.h, which
// defines None, breaking MediaPlayer::None enum
#if PLATFORM(X11) && GST_GL_HAVE_PLATFORM_EGL
#undef None
#endif // PLATFORM(X11) && GST_GL_HAVE_PLATFORM_EGL
#include "VideoTextureCopierGStreamer.h"
#endif // USE(GSTREAMER_GL)

#if USE(TEXTURE_MAPPER_GL)
#include "BitmapTextureGL.h"
#include "BitmapTexturePool.h"
#include "TextureMapperContextAttributes.h"
#include "TextureMapperPlatformLayerBuffer.h"
#include "TextureMapperPlatformLayerProxy.h"
#if USE(CAIRO) && ENABLE(ACCELERATED_2D_CANVAS)
#include <cairo-gl.h>
#endif
#endif // USE(TEXTURE_MAPPER_GL)

GST_DEBUG_CATEGORY(webkit_media_player_debug);
#define GST_CAT_DEFAULT webkit_media_player_debug

namespace WebCore {
using namespace std;

#if USE(GSTREAMER_HOLEPUNCH)
static const FloatSize s_holePunchDefaultFrameSize(1280, 720);
#endif

static int greatestCommonDivisor(int a, int b)
{
    while (b) {
        int temp = a;
        a = b;
        b = temp % b;
    }

    return ABS(a);
}

static void busMessageCallback(GstBus*, GstMessage* message, MediaPlayerPrivateGStreamer* player)
{
    player->handleMessage(message);
}

static void convertToInternalProtocol(URL& url)
{
    if (webkitGstCheckVersion(1, 12, 0))
        return;
    if (url.protocolIsInHTTPFamily() || url.protocolIsBlob())
        url.setProtocol("webkit+" + url.protocol());
}

#if USE(TEXTURE_MAPPER_GL)
class GstVideoFrameHolder : public TextureMapperPlatformLayerBuffer::UnmanagedBufferDataHolder {
public:
    explicit GstVideoFrameHolder(GstSample* sample, Optional<GstVideoDecoderPlatform> videoDecoderPlatform, TextureMapperGL::Flags flags, bool gstGLEnabled)
        : m_videoDecoderPlatform(videoDecoderPlatform)
    {
        RELEASE_ASSERT(GST_IS_SAMPLE(sample));

        GstVideoInfo videoInfo;
        if (UNLIKELY(!getSampleVideoInfo(sample, videoInfo)))
            return;

        m_size = IntSize(GST_VIDEO_INFO_WIDTH(&videoInfo), GST_VIDEO_INFO_HEIGHT(&videoInfo));
        m_hasAlphaChannel = GST_VIDEO_INFO_HAS_ALPHA(&videoInfo);
        m_buffer = gst_sample_get_buffer(sample);
        if (UNLIKELY(!GST_IS_BUFFER(m_buffer)))
            return;

#if USE(GSTREAMER_GL)
        m_flags = flags | (m_hasAlphaChannel ? TextureMapperGL::ShouldBlend : 0);

        if (gstGLEnabled) {
            m_isMapped = gst_video_frame_map(&m_videoFrame, &videoInfo, m_buffer, static_cast<GstMapFlags>(GST_MAP_READ | GST_MAP_GL));
            if (m_isMapped) {
                m_textureID = *reinterpret_cast<GLuint*>(m_videoFrame.data[0]);
                m_hasMappedTextures = true;
            }
        } else
#else
        UNUSED_PARAM(flags);
        UNUSED_PARAM(gstGLEnabled);
#endif // USE(GSTREAMER_GL)

        {
            m_textureID = 0;
            m_isMapped = gst_video_frame_map(&m_videoFrame, &videoInfo, m_buffer, GST_MAP_READ);
            if (m_isMapped) {
                // Right now the TextureMapper only supports chromas with one plane
                ASSERT(GST_VIDEO_INFO_N_PLANES(&videoInfo) == 1);
            }
        }
    }

    virtual ~GstVideoFrameHolder()
    {
        if (UNLIKELY(!m_isMapped))
            return;

        gst_video_frame_unmap(&m_videoFrame);
    }

#if USE(GSTREAMER_GL)
    virtual void waitForCPUSync()
    {
        GstGLSyncMeta* meta = gst_buffer_get_gl_sync_meta(m_buffer);
        if (meta) {
            GstMemory* mem = gst_buffer_peek_memory(m_buffer, 0);
            GstGLContext* context = ((GstGLBaseMemory*)mem)->context;
            gst_gl_sync_meta_wait_cpu(meta, context);
        }
    }
#endif // USE(GSTREAMER_GL)

    const IntSize& size() const { return m_size; }
    bool hasAlphaChannel() const { return m_hasAlphaChannel; }
    TextureMapperGL::Flags flags() const { return m_flags; }
    GLuint textureID() const { return m_textureID; }
    bool hasMappedTextures() const { return m_hasMappedTextures; }
    const GstVideoFrame& videoFrame() const { return m_videoFrame; }

    void updateTexture(BitmapTextureGL& texture)
    {
        ASSERT(!m_textureID);
        GstVideoGLTextureUploadMeta* meta;
        if (m_buffer && (meta = gst_buffer_get_video_gl_texture_upload_meta(m_buffer))) {
            if (meta->n_textures == 1) { // BRGx & BGRA formats use only one texture.
                guint ids[4] = { texture.id(), 0, 0, 0 };

                if (gst_video_gl_texture_upload_meta_upload(meta, ids))
                    return;
            }
        }

        if (!m_isMapped)
            return;

        int stride = GST_VIDEO_FRAME_PLANE_STRIDE(&m_videoFrame, 0);
        const void* srcData = GST_VIDEO_FRAME_PLANE_DATA(&m_videoFrame, 0);

        if (!srcData)
            return;

        texture.updateContents(srcData, WebCore::IntRect(0, 0, m_size.width(), m_size.height()), WebCore::IntPoint(0, 0), stride);
    }

    std::unique_ptr<TextureMapperPlatformLayerBuffer> platformLayerBuffer()
    {
        if (!m_hasMappedTextures)
            return nullptr;

        using Buffer = TextureMapperPlatformLayerBuffer;

        if ((GST_VIDEO_INFO_IS_RGB(&m_videoFrame.info) && GST_VIDEO_INFO_N_PLANES(&m_videoFrame.info) == 1))
            return makeUnique<Buffer>(Buffer::TextureVariant { Buffer::RGBTexture { *static_cast<GLuint*>(m_videoFrame.data[0]) } }, m_size, m_flags, GL_RGBA);

        if (GST_VIDEO_INFO_IS_YUV(&m_videoFrame.info)) {
            if (GST_VIDEO_INFO_N_COMPONENTS(&m_videoFrame.info) < 3 || GST_VIDEO_INFO_N_PLANES(&m_videoFrame.info) > 3)
                return nullptr;

            if (m_videoDecoderPlatform && *m_videoDecoderPlatform == GstVideoDecoderPlatform::ImxVPU) {
                // IMX VPU decoder decodes YUV data only into the Y texture from which the sampler
                // then directly produces RGBA data. Textures for other planes aren't used, but
                // that's decoder's problem. We have to treat that Y texture as having RGBA data.
                return makeUnique<Buffer>(Buffer::TextureVariant { Buffer::RGBTexture { *static_cast<GLuint*>(m_videoFrame.data[0]) } }, m_size, m_flags, GL_RGBA);
            }

            unsigned numberOfPlanes = GST_VIDEO_INFO_N_PLANES(&m_videoFrame.info);
            std::array<GLuint, 3> planes;
            std::array<unsigned, 3> yuvPlane;
            std::array<unsigned, 3> yuvPlaneOffset;
            for (unsigned i = 0; i < numberOfPlanes; ++i)
                planes[i] = *static_cast<GLuint*>(m_videoFrame.data[i]);
            for (unsigned i = 0; i < 3; ++i) {
                yuvPlane[i] = GST_VIDEO_INFO_COMP_PLANE(&m_videoFrame.info, i);
                yuvPlaneOffset[i] = GST_VIDEO_INFO_COMP_POFFSET(&m_videoFrame.info, i);
            }

            std::array<GLfloat, 9> yuvToRgb;
            if (gst_video_colorimetry_matches(&GST_VIDEO_INFO_COLORIMETRY(&m_videoFrame.info), GST_VIDEO_COLORIMETRY_BT709)) {
                yuvToRgb = {
                    1.164f,  0.0f,    1.787f,
                    1.164f, -0.213f, -0.531f,
                    1.164f,  2.112f,  0.0f
                };
            } else {
                // Default to bt601. This is the same behaviour as GStreamer's glcolorconvert element.
                yuvToRgb = {
                    1.164f,  0.0f,    1.596f,
                    1.164f, -0.391f, -0.813f,
                    1.164f,  2.018f,  0.0f
                };
            }

            return makeUnique<Buffer>( Buffer::TextureVariant { Buffer::YUVTexture { numberOfPlanes, planes, yuvPlane, yuvPlaneOffset, yuvToRgb } }, m_size, m_flags, GL_RGBA);
        }

        return nullptr;
    }

private:
    GstBuffer* m_buffer;
    GstVideoFrame m_videoFrame { };
    IntSize m_size;
    bool m_hasAlphaChannel;
    Optional<GstVideoDecoderPlatform> m_videoDecoderPlatform;
    TextureMapperGL::Flags m_flags { };
    GLuint m_textureID { 0 };
    bool m_isMapped { false };
    bool m_hasMappedTextures { false };
};
#endif

void MediaPlayerPrivateGStreamer::initializeDebugCategory()
{
    GST_DEBUG_CATEGORY_INIT(webkit_media_player_debug, "webkitmediaplayer", 0, "WebKit media player");
}

MediaPlayerPrivateGStreamer::MediaPlayerPrivateGStreamer(MediaPlayer* player)
    : m_notifier(MainThreadNotifier<MainThreadNotification>::create())
    , m_player(player)
    , m_cachedPosition(MediaTime::invalidTime())
    , m_cachedDuration(MediaTime::invalidTime())
    , m_seekTime(MediaTime::invalidTime())
    , m_timeOfOverlappingSeek(MediaTime::invalidTime())
    , m_fillTimer(*this, &MediaPlayerPrivateGStreamer::fillTimerFired)
    , m_maxTimeLoaded(MediaTime::zeroTime())
    , m_preload(player->preload())
    , m_maxTimeLoadedAtLastDidLoadingProgress(MediaTime::zeroTime())
    , m_drawTimer(RunLoop::main(), this, &MediaPlayerPrivateGStreamer::repaint)
    , m_readyTimerHandler(RunLoop::main(), this, &MediaPlayerPrivateGStreamer::readyTimerFired
)
#if USE(TEXTURE_MAPPER_GL)
#if USE(NICOSIA)
    , m_nicosiaLayer(Nicosia::ContentLayer::create(Nicosia::ContentLayerTextureMapperImpl::createFactory(*this)))
#else
    , m_platformLayerProxy(adoptRef(new TextureMapperPlatformLayerProxy()))
#endif
#endif      
{
#if USE(GLIB)
    m_readyTimerHandler.setPriority(G_PRIORITY_DEFAULT_IDLE);
#endif
}

MediaPlayerPrivateGStreamer::~MediaPlayerPrivateGStreamer()
{
    GST_DEBUG_OBJECT(pipeline(), "Disposing player");

#if ENABLE(VIDEO_TRACK)
    for (auto& track : m_audioTracks.values())
        track->disconnect();

    for (auto& track : m_textTracks.values())
        track->disconnect();

    for (auto& track : m_videoTracks.values())
        track->disconnect();
#endif
    if (m_fillTimer.isActive())
        m_fillTimer.stop();

    if (m_mediaLocations) {
        gst_structure_free(m_mediaLocations);
        m_mediaLocations = nullptr;
    }

    if (WEBKIT_IS_WEB_SRC(m_source.get()) && GST_OBJECT_PARENT(m_source.get()))
        g_signal_handlers_disconnect_by_func(GST_ELEMENT_PARENT(m_source.get()), reinterpret_cast<gpointer>(uriDecodeBinElementAddedCallback), this);

    if (m_autoAudioSink) {
        g_signal_handlers_disconnect_by_func(G_OBJECT(m_autoAudioSink.get()),
            reinterpret_cast<gpointer>(setAudioStreamPropertiesCallback), this);
    }

    m_readyTimerHandler.stop();
    for (auto& missingPluginCallback : m_missingPluginCallbacks) {
        if (missingPluginCallback)
            missingPluginCallback->invalidate();
    }
    m_missingPluginCallbacks.clear();

    if (m_videoSink) {
        GRefPtr<GstPad> videoSinkPad = adoptGRef(gst_element_get_static_pad(m_videoSink.get(), "sink"));
        g_signal_handlers_disconnect_matched(videoSinkPad.get(), G_SIGNAL_MATCH_DATA, 0, 0, nullptr, nullptr, this);
    }

    if (m_pipeline) {
        GRefPtr<GstBus> bus = adoptGRef(gst_pipeline_get_bus(GST_PIPELINE(m_pipeline.get())));
        ASSERT(bus);
        g_signal_handlers_disconnect_by_func(bus.get(), gpointer(busMessageCallback), this);
        gst_bus_remove_signal_watch(bus.get());
        gst_bus_set_sync_handler(bus.get(), nullptr, nullptr, nullptr);
        g_signal_handlers_disconnect_matched(m_pipeline.get(), G_SIGNAL_MATCH_DATA, 0, 0, nullptr, nullptr, this);
    }

#if USE(GSTREAMER_GL)
    if (m_videoDecoderPlatform == GstVideoDecoderPlatform::Video4Linux)
        flushCurrentBuffer();
#endif
#if USE(TEXTURE_MAPPER_GL) && USE(NICOSIA)
    downcast<Nicosia::ContentLayerTextureMapperImpl>(m_nicosiaLayer->impl()).invalidateClient();
#endif

    m_notifier->invalidate();

    if (m_videoSink) {
        g_signal_handlers_disconnect_matched(m_videoSink.get(), G_SIGNAL_MATCH_DATA, 0, 0, nullptr, nullptr, this);
#if USE(GSTREAMER_GL)
        if (GST_IS_BIN(m_videoSink.get())) {
            GRefPtr<GstElement> appsink = adoptGRef(gst_bin_get_by_name(GST_BIN_CAST(m_videoSink.get()), "webkit-gl-video-sink"));
            g_signal_handlers_disconnect_by_data(appsink.get(), this);
        }
#endif
    }

    if (m_volumeElement)
        g_signal_handlers_disconnect_matched(m_volumeElement.get(), G_SIGNAL_MATCH_DATA, 0, 0, nullptr, nullptr, this);

    // This will release the GStreamer thread from m_drawCondition in non AC mode in case there's an ongoing triggerRepaint call
    // waiting there, and ensure that any triggerRepaint call reaching the lock won't wait on m_drawCondition.
    cancelRepaint(true);

#if ENABLE(ENCRYPTED_MEDIA)
    m_cdmAttachmentSemaphore.signal();
#endif

    // The change to GST_STATE_NULL state is always synchronous. So after this gets executed we don't need to worry
    // about handlers running in the GStreamer thread.
    if (m_pipeline)
        gst_element_set_state(m_pipeline.get(), GST_STATE_NULL);

    m_player = nullptr;
}

bool MediaPlayerPrivateGStreamer::isAvailable()
{
    if (!initializeGStreamerAndRegisterWebKitElements())
        return false;

    // FIXME: This has not been updated for the playbin3 switch.
    GRefPtr<GstElementFactory> factory = adoptGRef(gst_element_factory_find("playbin"));
    return factory;
}

void MediaPlayerPrivateGStreamer::registerMediaEngine(MediaEngineRegistrar registrar)
{
    initializeDebugCategory();

    if (isAvailable()) {
        registrar([](MediaPlayer* player) { return makeUnique<MediaPlayerPrivateGStreamer>(player); },
            getSupportedTypes, supportsType, nullptr, nullptr, nullptr, supportsKeySystem);
    }
}

void MediaPlayerPrivateGStreamer::loadFull(const String& urlString, const String& pipelineName)
{
    if (m_player->contentMIMEType() == "image/gif") {
        loadingFailed(MediaPlayer::FormatError, MediaPlayer::HaveNothing, true);
        return;
    }

    URL url(URL(), urlString);
    if (url.protocolIsAbout()) {
        loadingFailed(MediaPlayer::FormatError, MediaPlayer::HaveNothing, true);
        return;
    }

    if (!m_pipeline)
        createGSTPlayBin(url, pipelineName);
    syncOnClock(true);
    if (m_fillTimer.isActive())
        m_fillTimer.stop();

    ASSERT(m_pipeline);

    setPlaybinURL(url);

    GST_DEBUG_OBJECT(pipeline(), "preload: %s", convertEnumerationToString(m_preload).utf8().data());
    if (m_preload == MediaPlayer::None) {
        GST_INFO_OBJECT(pipeline(), "Delaying load.");
        m_delayingLoad = true;
    }

    // Reset network and ready states. Those will be set properly once
    // the pipeline pre-rolled.
    m_networkState = MediaPlayer::Loading;
    m_player->networkStateChanged();
    m_readyState = MediaPlayer::HaveNothing;
    m_player->readyStateChanged();
    m_volumeAndMuteInitialized = false;
    m_hasTaintedOrigin = WTF::nullopt;

    if (!m_delayingLoad)
        commitLoad();
}

void MediaPlayerPrivateGStreamer::load(const String& urlString)
{
    loadFull(urlString, String());
}

#if ENABLE(MEDIA_SOURCE)
void MediaPlayerPrivateGStreamer::load(const String&, MediaSourcePrivateClient*)
{
    // Properly fail so the global MediaPlayer tries to fallback to the next MediaPlayerPrivate.
    m_networkState = MediaPlayer::FormatError;
    m_player->networkStateChanged();
}
#endif

#if ENABLE(MEDIA_STREAM)
void MediaPlayerPrivateGStreamer::load(MediaStreamPrivate& stream)
{
    m_streamPrivate = &stream;
    static Atomic<uint32_t> pipelineId;
    auto pipelineName = makeString("mediastream-", pipelineId.exchangeAdd(1));

    loadFull(String("mediastream://") + stream.id(), pipelineName);
    syncOnClock(false);

#if USE(GSTREAMER_GL)
    ensureGLVideoSinkContext();
#endif
    m_player->play();
}
#endif

void MediaPlayerPrivateGStreamer::commitLoad()
{
    ASSERT(!m_delayingLoad);
    GST_DEBUG_OBJECT(pipeline(), "Committing load.");

    // GStreamer needs to have the pipeline set to a paused state to
    // start providing anything useful.
    changePipelineState(GST_STATE_PAUSED);

    updateDownloadBufferingFlag();
    updateStates();
}

void MediaPlayerPrivateGStreamer::cancelLoad()
{
    if (m_networkState < MediaPlayer::Loading || m_networkState == MediaPlayer::Loaded)
        return;

    if (m_pipeline)
        changePipelineState(GST_STATE_READY);
}

void MediaPlayerPrivateGStreamer::prepareToPlay()
{
    GST_DEBUG_OBJECT(pipeline(), "Prepare to play");
    m_preload = MediaPlayer::Auto;
    if (m_delayingLoad) {
        m_delayingLoad = false;
        commitLoad();
    }
}

void MediaPlayerPrivateGStreamer::play()
{
    if (!m_playbackRate) {
        m_playbackRatePause = true;
        return;
    }

    if (changePipelineState(GST_STATE_PLAYING)) {
        m_isEndReached = false;
        m_delayingLoad = false;
        m_preload = MediaPlayer::Auto;
        updateDownloadBufferingFlag();
        GST_INFO_OBJECT(pipeline(), "Play");
    } else
        loadingFailed(MediaPlayer::Empty);
}

void MediaPlayerPrivateGStreamer::pause()
{
    m_playbackRatePause = false;
    GstState currentState, pendingState;
    gst_element_get_state(m_pipeline.get(), &currentState, &pendingState, 0);
    if (currentState < GST_STATE_PAUSED && pendingState <= GST_STATE_PAUSED)
        return;

    if (changePipelineState(GST_STATE_PAUSED))
        GST_INFO_OBJECT(pipeline(), "Pause");
    else
        loadingFailed(MediaPlayer::Empty);
}

bool MediaPlayerPrivateGStreamer::paused() const
{
    if (!m_pipeline)
        return true;

    if (m_isEndReached) {
        GST_DEBUG_OBJECT(pipeline(), "Ignoring pause at EOS");
        return true;
    }

    if (m_playbackRatePause) {
        GST_DEBUG_OBJECT(pipeline(), "Playback rate is 0, simulating PAUSED state");
        return false;
    }

    GstState state;
    gst_element_get_state(m_pipeline.get(), &state, nullptr, 0);
    bool paused = state <= GST_STATE_PAUSED;
    GST_LOG_OBJECT(pipeline(), "Paused: %s", toString(paused).utf8().data());
    return paused;
}

bool MediaPlayerPrivateGStreamer::doSeek(const MediaTime& position, float rate, GstSeekFlags seekType)
{
    // Default values for rate >= 0.
    MediaTime startTime = position, endTime = MediaTime::invalidTime();

    if (rate < 0) {
        startTime = MediaTime::zeroTime();
        // If we are at beginning of media, start from the end to
        // avoid immediate EOS.
        if (position < MediaTime::zeroTime())
            endTime = durationMediaTime();
        else
            endTime = position;
    }

    if (!rate)
        rate = 1.0;

    return gst_element_seek(m_pipeline.get(), rate, GST_FORMAT_TIME, seekType,
        GST_SEEK_TYPE_SET, toGstClockTime(startTime), GST_SEEK_TYPE_SET, toGstClockTime(endTime));
}

void MediaPlayerPrivateGStreamer::seek(const MediaTime& mediaTime)
{
    if (!m_pipeline)
        return;

    if (m_errorOccured)
        return;

    GST_INFO_OBJECT(pipeline(), "[Seek] seek attempt to %s", toString(mediaTime).utf8().data());

    // Avoid useless seeking.
    if (mediaTime == currentMediaTime()) {
        GST_DEBUG_OBJECT(pipeline(), "[Seek] seek to EOS position unhandled");
        return;
    }

    MediaTime time = std::min(mediaTime, durationMediaTime());

    if (isLiveStream()) {
        GST_DEBUG_OBJECT(pipeline(), "[Seek] Live stream seek unhandled");
        return;
    }

    GST_INFO_OBJECT(pipeline(), "[Seek] seeking to %s", toString(time).utf8().data());

    if (m_seeking) {
        m_timeOfOverlappingSeek = time;
        if (m_seekIsPending) {
            m_seekTime = time;
            return;
        }
    }

    GstState state;
    GstStateChangeReturn getStateResult = gst_element_get_state(m_pipeline.get(), &state, nullptr, 0);
    if (getStateResult == GST_STATE_CHANGE_FAILURE || getStateResult == GST_STATE_CHANGE_NO_PREROLL) {
        GST_DEBUG_OBJECT(pipeline(), "[Seek] cannot seek, current state change is %s", gst_element_state_change_return_get_name(getStateResult));
        return;
    }
    if (getStateResult == GST_STATE_CHANGE_ASYNC || state < GST_STATE_PAUSED || m_isEndReached) {
        m_seekIsPending = true;
        if (m_isEndReached) {
            GST_DEBUG_OBJECT(pipeline(), "[Seek] reset pipeline");
            m_resetPipeline = true;
            if (!changePipelineState(GST_STATE_PAUSED))
                loadingFailed(MediaPlayer::Empty);
        }
    } else {
        // We can seek now.
        if (!doSeek(time, m_player->rate(), static_cast<GstSeekFlags>(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_ACCURATE))) {
            GST_DEBUG_OBJECT(pipeline(), "[Seek] seeking to %s failed", toString(time).utf8().data());
            return;
        }
    }

    m_seeking = true;
    m_seekTime = time;
    m_isEndReached = false;
}

void MediaPlayerPrivateGStreamer::updatePlaybackRate()
{
    if (!m_changingRate)
        return;

    GST_INFO_OBJECT(pipeline(), "Set Rate to %f", m_playbackRate);

    // Mute the sound if the playback rate is negative or too extreme and audio pitch is not adjusted.
    bool mute = m_playbackRate <= 0 || (!m_preservesPitch && (m_playbackRate < 0.8 || m_playbackRate > 2));

    GST_INFO_OBJECT(pipeline(), mute ? "Need to mute audio" : "Do not need to mute audio");

    if (doSeek(playbackPosition(), m_playbackRate, static_cast<GstSeekFlags>(GST_SEEK_FLAG_FLUSH))) {
        g_object_set(m_pipeline.get(), "mute", mute, nullptr);
        m_lastPlaybackRate = m_playbackRate;
    } else {
        m_playbackRate = m_lastPlaybackRate;
        GST_ERROR("Set rate to %f failed", m_playbackRate);
    }

    if (m_playbackRatePause) {
        GstState state;
        GstState pending;

        gst_element_get_state(m_pipeline.get(), &state, &pending, 0);
        if (state != GST_STATE_PLAYING && pending != GST_STATE_PLAYING)
            changePipelineState(GST_STATE_PLAYING);
        m_playbackRatePause = false;
    }

    m_changingRate = false;
    m_player->rateChanged();
}

MediaTime MediaPlayerPrivateGStreamer::platformDuration() const
{
    if (!m_pipeline)
        return MediaTime::invalidTime();

    GST_TRACE_OBJECT(pipeline(), "errorOccured: %s, pipeline state: %s", boolForPrinting(m_errorOccured), gst_element_state_get_name(GST_STATE(m_pipeline.get())));
    if (m_errorOccured)
        return MediaTime::invalidTime();

    // The duration query would fail on a not-prerolled pipeline.
    if (GST_STATE(m_pipeline.get()) < GST_STATE_PAUSED)
        return MediaTime::invalidTime();

    int64_t duration = 0;
    if (!gst_element_query_duration(m_pipeline.get(), GST_FORMAT_TIME, &duration) || !GST_CLOCK_TIME_IS_VALID(duration)) {
        GST_DEBUG_OBJECT(pipeline(), "Time duration query failed for %s", m_url.string().utf8().data());
        return MediaTime::positiveInfiniteTime();
    }

    GST_LOG_OBJECT(pipeline(), "Duration: %" GST_TIME_FORMAT, GST_TIME_ARGS(duration));
    return MediaTime(duration, GST_SECOND);
}

MediaTime MediaPlayerPrivateGStreamer::durationMediaTime() const
{
    GST_TRACE_OBJECT(pipeline(), "Cached duration: %s", m_cachedDuration.toString().utf8().data());
    if (m_cachedDuration.isValid())
        return m_cachedDuration;

    MediaTime duration = platformDuration();
    if (!duration || duration.isInvalid())
        return MediaTime::zeroTime();

    m_cachedDuration = duration;

    return m_cachedDuration;
}

MediaTime MediaPlayerPrivateGStreamer::currentMediaTime() const
{
    if (!m_pipeline || m_errorOccured)
        return MediaTime::invalidTime();

    GST_TRACE_OBJECT(pipeline(), "seeking: %s, seekTime: %s", boolForPrinting(m_seeking), m_seekTime.toString().utf8().data());
    if (m_seeking)
        return m_seekTime;

    return playbackPosition();
}

void MediaPlayerPrivateGStreamer::setRate(float rate)
{
    // Higher rate causes crash.
    rate = clampTo(rate, -20.0, 20.0);

    // Avoid useless playback rate update.
    if (m_playbackRate == rate) {
        // and make sure that upper layers were notified if rate was set

        if (!m_changingRate && m_player->rate() != m_playbackRate)
            m_player->rateChanged();
        return;
    }

    if (isLiveStream()) {
        // notify upper layers that we cannot handle passed rate.
        m_changingRate = false;
        m_player->rateChanged();
        return;
    }

    GstState state;
    GstState pending;

    m_playbackRate = rate;
    m_changingRate = true;

    gst_element_get_state(m_pipeline.get(), &state, &pending, 0);

    if (!rate) {
        m_changingRate = false;
        m_playbackRatePause = true;
        if (state != GST_STATE_PAUSED && pending != GST_STATE_PAUSED)
            changePipelineState(GST_STATE_PAUSED);
        return;
    }

    if ((state != GST_STATE_PLAYING && state != GST_STATE_PAUSED)
        || (pending == GST_STATE_PAUSED))
        return;

    updatePlaybackRate();
}

double MediaPlayerPrivateGStreamer::rate() const
{
    return m_playbackRate;
}

void MediaPlayerPrivateGStreamer::setPreservesPitch(bool preservesPitch)
{
    m_preservesPitch = preservesPitch;
}

void MediaPlayerPrivateGStreamer::setPreload(MediaPlayer::Preload preload)
{
    GST_DEBUG_OBJECT(pipeline(), "Setting preload to %s", convertEnumerationToString(preload).utf8().data());
    if (preload == MediaPlayer::Auto && isLiveStream())
        return;

    m_preload = preload;
    updateDownloadBufferingFlag();

    if (m_delayingLoad && m_preload != MediaPlayer::None) {
        m_delayingLoad = false;
        commitLoad();
    }
}

std::unique_ptr<PlatformTimeRanges> MediaPlayerPrivateGStreamer::buffered() const
{
    auto timeRanges = makeUnique<PlatformTimeRanges>();
    if (m_errorOccured || isLiveStream())
        return timeRanges;

    MediaTime mediaDuration = durationMediaTime();
    if (!mediaDuration || mediaDuration.isPositiveInfinite())
        return timeRanges;

    GRefPtr<GstQuery> query = adoptGRef(gst_query_new_buffering(GST_FORMAT_PERCENT));

    if (!gst_element_query(m_pipeline.get(), query.get()))
        return timeRanges;

    guint numBufferingRanges = gst_query_get_n_buffering_ranges(query.get());
    for (guint index = 0; index < numBufferingRanges; index++) {
        gint64 rangeStart = 0, rangeStop = 0;
        if (gst_query_parse_nth_buffering_range(query.get(), index, &rangeStart, &rangeStop)) {
            uint64_t startTime = gst_util_uint64_scale_int_round(toGstUnsigned64Time(mediaDuration), rangeStart, GST_FORMAT_PERCENT_MAX);
            uint64_t stopTime = gst_util_uint64_scale_int_round(toGstUnsigned64Time(mediaDuration), rangeStop, GST_FORMAT_PERCENT_MAX);
            timeRanges->add(MediaTime(startTime, GST_SECOND), MediaTime(stopTime, GST_SECOND));
        }
    }

    // Fallback to the more general maxTimeLoaded() if no range has
    // been found.
    if (!timeRanges->length()) {
        MediaTime loaded = maxTimeLoaded();
        if (loaded.isValid() && loaded)
            timeRanges->add(MediaTime::zeroTime(), loaded);
    }

    return timeRanges;
}

MediaTime MediaPlayerPrivateGStreamer::maxMediaTimeSeekable() const
{
    GST_TRACE_OBJECT(pipeline(), "errorOccured: %s, isLiveStream: %s", boolForPrinting(m_errorOccured), boolForPrinting(isLiveStream()));
    if (m_errorOccured)
        return MediaTime::zeroTime();

    if (isLiveStream())
        return MediaTime::zeroTime();

    MediaTime duration = durationMediaTime();
    GST_DEBUG_OBJECT(pipeline(), "maxMediaTimeSeekable, duration: %s", toString(duration).utf8().data());
    // infinite duration means live stream
    if (duration.isPositiveInfinite())
        return MediaTime::zeroTime();

    return duration;
}

MediaTime MediaPlayerPrivateGStreamer::maxTimeLoaded() const
{
    if (m_errorOccured)
        return MediaTime::zeroTime();

    MediaTime loaded = m_maxTimeLoaded;
    if (m_isEndReached)
        loaded = durationMediaTime();
    GST_LOG_OBJECT(pipeline(), "maxTimeLoaded: %s", toString(loaded).utf8().data());
    return loaded;
}

bool MediaPlayerPrivateGStreamer::didLoadingProgress() const
{
    if (m_errorOccured || m_loadingStalled)
        return false;

    if (WEBKIT_IS_WEB_SRC(m_source.get())) {
        GST_LOG_OBJECT(pipeline(), "Last network read position: %" G_GUINT64_FORMAT ", current: %" G_GUINT64_FORMAT, m_readPositionAtLastDidLoadingProgress, m_networkReadPosition);
        bool didLoadingProgress = m_readPositionAtLastDidLoadingProgress < m_networkReadPosition;
        m_readPositionAtLastDidLoadingProgress = m_networkReadPosition;
        GST_LOG_OBJECT(pipeline(), "didLoadingProgress: %s", boolForPrinting(didLoadingProgress));
        return didLoadingProgress;
    }

    if (UNLIKELY(!m_pipeline || !durationMediaTime() || (!isMediaSource() && !totalBytes())))
        return false;

    MediaTime currentMaxTimeLoaded = maxTimeLoaded();
    bool didLoadingProgress = currentMaxTimeLoaded != m_maxTimeLoadedAtLastDidLoadingProgress;
    m_maxTimeLoadedAtLastDidLoadingProgress = currentMaxTimeLoaded;
    GST_LOG_OBJECT(pipeline(), "didLoadingProgress: %s", boolForPrinting(didLoadingProgress));
    return didLoadingProgress;
}

unsigned long long MediaPlayerPrivateGStreamer::totalBytes() const
{
    if (m_errorOccured)
        return 0;

    if (m_totalBytes)
        return m_totalBytes;

    if (!m_source)
        return 0;

    if (isLiveStream())
        return 0;

    GstFormat fmt = GST_FORMAT_BYTES;
    gint64 length = 0;
    if (gst_element_query_duration(m_source.get(), fmt, &length)) {
        GST_INFO_OBJECT(pipeline(), "totalBytes %" G_GINT64_FORMAT, length);
        m_totalBytes = static_cast<unsigned long long>(length);
        m_isStreaming = !length;
        return m_totalBytes;
    }

    // Fall back to querying the source pads manually.
    // See also https://bugzilla.gnome.org/show_bug.cgi?id=638749
    GstIterator* iter = gst_element_iterate_src_pads(m_source.get());
    bool done = false;
    while (!done) {
        GValue item = G_VALUE_INIT;
        switch (gst_iterator_next(iter, &item)) {
        case GST_ITERATOR_OK: {
            GstPad* pad = static_cast<GstPad*>(g_value_get_object(&item));
            gint64 padLength = 0;
            if (gst_pad_query_duration(pad, fmt, &padLength) && padLength > length)
                length = padLength;
            break;
        }
        case GST_ITERATOR_RESYNC:
            gst_iterator_resync(iter);
            break;
        case GST_ITERATOR_ERROR:
            FALLTHROUGH;
        case GST_ITERATOR_DONE:
            done = true;
            break;
        }

        g_value_unset(&item);
    }

    gst_iterator_free(iter);

    GST_INFO_OBJECT(pipeline(), "totalBytes %" G_GINT64_FORMAT, length);
    m_totalBytes = static_cast<unsigned long long>(length);
    m_isStreaming = !length;
    return m_totalBytes;
}

bool MediaPlayerPrivateGStreamer::hasSingleSecurityOrigin() const
{
    if (!m_source)
        return false;

    if (!WEBKIT_IS_WEB_SRC(m_source.get()))
        return true;

    GUniqueOutPtr<char> originalURI, resolvedURI;
    g_object_get(m_source.get(), "location", &originalURI.outPtr(), "resolved-location", &resolvedURI.outPtr(), nullptr);
    if (!originalURI || !resolvedURI)
        return false;
    if (!g_strcmp0(originalURI.get(), resolvedURI.get()))
        return true;

    Ref<SecurityOrigin> resolvedOrigin(SecurityOrigin::createFromString(String::fromUTF8(resolvedURI.get())));
    Ref<SecurityOrigin> requestedOrigin(SecurityOrigin::createFromString(String::fromUTF8(originalURI.get())));
    return resolvedOrigin->isSameSchemeHostPort(requestedOrigin.get());
}

Optional<bool> MediaPlayerPrivateGStreamer::wouldTaintOrigin(const SecurityOrigin& origin) const
{
    if (webkitGstCheckVersion(1, 12, 0)) {
        GST_TRACE_OBJECT(pipeline(), "Checking %u origins", m_origins.size());
        for (auto& responseOrigin : m_origins) {
            if (!origin.canAccess(*responseOrigin)) {
                GST_DEBUG_OBJECT(pipeline(), "Found reachable response origin");
                return true;
            }
        }
        GST_DEBUG_OBJECT(pipeline(), "No valid response origin found");
        return false;
    }

    // GStreamer < 1.12 has an incomplete uridownloader implementation so we
    // can't use WebKitWebSrc for adaptive fragments downloading if this
    // version is detected.
    UNUSED_PARAM(origin);
    return m_hasTaintedOrigin;
}

void MediaPlayerPrivateGStreamer::simulateAudioInterruption()
{
    GstMessage* message = gst_message_new_request_state(GST_OBJECT(m_pipeline.get()), GST_STATE_PAUSED);
    gst_element_post_message(m_pipeline.get(), message);
}

#if ENABLE(WEB_AUDIO)
void MediaPlayerPrivateGStreamer::ensureAudioSourceProvider()
{
    if (!m_audioSourceProvider)
        m_audioSourceProvider = makeUnique<AudioSourceProviderGStreamer>();
}

AudioSourceProvider* MediaPlayerPrivateGStreamer::audioSourceProvider()
{
    ensureAudioSourceProvider();
    return m_audioSourceProvider.get();
}
#endif

void MediaPlayerPrivateGStreamer::durationChanged()
{
    MediaTime previousDuration = durationMediaTime();
    m_cachedDuration = MediaTime::invalidTime();

    // Avoid emiting durationchanged in the case where the previous
    // duration was 0 because that case is already handled by the
    // HTMLMediaElement.
    if (previousDuration && durationMediaTime() != previousDuration)
        m_player->durationChanged();
}

void MediaPlayerPrivateGStreamer::sourceSetup(GstElement* sourceElement)
{
    GST_DEBUG_OBJECT(pipeline(), "Source element set-up for %s", GST_ELEMENT_NAME(sourceElement));

    if (WEBKIT_IS_WEB_SRC(m_source.get()) && GST_OBJECT_PARENT(m_source.get()))
        g_signal_handlers_disconnect_by_func(GST_ELEMENT_PARENT(m_source.get()), reinterpret_cast<gpointer>(uriDecodeBinElementAddedCallback), this);

    m_source = sourceElement;

    if (WEBKIT_IS_WEB_SRC(m_source.get())) {
        webKitWebSrcSetMediaPlayer(WEBKIT_WEB_SRC_CAST(m_source.get()), m_player);
        g_signal_connect(GST_ELEMENT_PARENT(m_source.get()), "element-added", G_CALLBACK(uriDecodeBinElementAddedCallback), this);
#if ENABLE(MEDIA_STREAM)
    } else if (WEBKIT_IS_MEDIA_STREAM_SRC(sourceElement)) {
        auto stream = m_streamPrivate.get();
        ASSERT(stream);
        webkitMediaStreamSrcSetStream(WEBKIT_MEDIA_STREAM_SRC(sourceElement), stream);
#endif
    }
}

void MediaPlayerPrivateGStreamer::setAudioStreamPropertiesCallback(MediaPlayerPrivateGStreamer* player, GObject* object)
{
    player->setAudioStreamProperties(object);
}

void MediaPlayerPrivateGStreamer::setAudioStreamProperties(GObject* object)
{
    if (g_strcmp0(G_OBJECT_TYPE_NAME(object), "GstPulseSink"))
        return;

    const char* role = m_player->client().mediaPlayerIsVideo() ? "video" : "music";
    GstStructure* structure = gst_structure_new("stream-properties", "media.role", G_TYPE_STRING, role, nullptr);
    g_object_set(object, "stream-properties", structure, nullptr);
    gst_structure_free(structure);
    GUniquePtr<gchar> elementName(gst_element_get_name(GST_ELEMENT(object)));
    GST_DEBUG_OBJECT(pipeline(), "Set media.role as %s at %s", role, elementName.get());
}

void MediaPlayerPrivateGStreamer::sourceSetupCallback(MediaPlayerPrivateGStreamer* player, GstElement* sourceElement)
{
    player->sourceSetup(sourceElement);
}

bool MediaPlayerPrivateGStreamer::changePipelineState(GstState newState)
{
    ASSERT(m_pipeline);

    GstState currentState;
    GstState pending;

    gst_element_get_state(m_pipeline.get(), &currentState, &pending, 0);
    if (currentState == newState || pending == newState) {
        GST_DEBUG_OBJECT(pipeline(), "Rejected state change to %s from %s with %s pending", gst_element_state_get_name(newState),
            gst_element_state_get_name(currentState), gst_element_state_get_name(pending));
        return true;
    }

    GST_DEBUG_OBJECT(pipeline(), "Changing state change to %s from %s with %s pending", gst_element_state_get_name(newState),
        gst_element_state_get_name(currentState), gst_element_state_get_name(pending));

#if USE(GSTREAMER_GL)
    if (currentState <= GST_STATE_READY && newState >= GST_STATE_PAUSED)
        ensureGLVideoSinkContext();
#endif

    GstStateChangeReturn setStateResult = gst_element_set_state(m_pipeline.get(), newState);
    GstState pausedOrPlaying = newState == GST_STATE_PLAYING ? GST_STATE_PAUSED : GST_STATE_PLAYING;
    if (currentState != pausedOrPlaying && setStateResult == GST_STATE_CHANGE_FAILURE)
        return false;

    // Create a timer when entering the READY state so that we can free resources
    // if we stay for too long on READY.
    // Also lets remove the timer if we request a state change for any state other than READY.
    // See also https://bugs.webkit.org/show_bug.cgi?id=117354
    if (newState == GST_STATE_READY && !m_readyTimerHandler.isActive()) {
        // Max interval in seconds to stay in the READY state on manual
        // state change requests.
        static const Seconds readyStateTimerDelay { 1_min };
        m_readyTimerHandler.startOneShot(readyStateTimerDelay);
    } else if (newState != GST_STATE_READY)
        m_readyTimerHandler.stop();

    return true;
}

void MediaPlayerPrivateGStreamer::setPlaybinURL(const URL& url)
{
    // Clean out everything after file:// url path.
    String cleanURLString(url.string());
    if (url.isLocalFile())
        cleanURLString = cleanURLString.substring(0, url.pathEnd());

    m_url = URL(URL(), cleanURLString);
    convertToInternalProtocol(m_url);
    GST_INFO_OBJECT(pipeline(), "Load %s", m_url.string().utf8().data());
    g_object_set(m_pipeline.get(), "uri", m_url.string().utf8().data(), nullptr);
}

static void setSyncOnClock(GstElement *element, bool sync)
{
    if (!GST_IS_BIN(element)) {
        g_object_set(element, "sync", sync, NULL);
        return;
    }

    GstIterator* it = gst_bin_iterate_sinks(GST_BIN(element));
    while (gst_iterator_foreach(it, (GstIteratorForeachFunction)([](const GValue* item, void* syncPtr) {
        bool* sync = static_cast<bool*>(syncPtr);
        setSyncOnClock(GST_ELEMENT(g_value_get_object(item)), *sync);
    }), &sync) == GST_ITERATOR_RESYNC)
        gst_iterator_resync(it);
    gst_iterator_free(it);
}

void MediaPlayerPrivateGStreamer::syncOnClock(bool sync)
{
    setSyncOnClock(videoSink(), sync);
    setSyncOnClock(audioSink(), sync);
}

void MediaPlayerPrivateGStreamer::notifyPlayerOfVideo()
{
    if (UNLIKELY(!m_pipeline || !m_source))
        return;

    ASSERT(m_isLegacyPlaybin || isMediaSource());

    gint numTracks = 0;
    bool useMediaSource = isMediaSource();
    GstElement* element = useMediaSource ? m_source.get() : m_pipeline.get();
    g_object_get(element, "n-video", &numTracks, nullptr);

    GST_INFO_OBJECT(pipeline(), "Media has %d video tracks", numTracks);

    bool oldHasVideo = m_hasVideo;
    m_hasVideo = numTracks > 0;
    if (oldHasVideo != m_hasVideo)
        m_player->characteristicChanged();

    if (m_hasVideo)
        m_player->sizeChanged();

    if (useMediaSource) {
        GST_DEBUG_OBJECT(pipeline(), "Tracks managed by source element. Bailing out now.");
        m_player->client().mediaPlayerEngineUpdated(m_player);
        return;
    }

#if ENABLE(VIDEO_TRACK)
    Vector<String> validVideoStreams;
    for (gint i = 0; i < numTracks; ++i) {
        GRefPtr<GstPad> pad;
        g_signal_emit_by_name(m_pipeline.get(), "get-video-pad", i, &pad.outPtr(), nullptr);
        ASSERT(pad);

        String streamId = "V" + String::number(i);
        validVideoStreams.append(streamId);
        if (i < static_cast<gint>(m_videoTracks.size())) {
            RefPtr<VideoTrackPrivateGStreamer> existingTrack = m_videoTracks.get(streamId);
            if (existingTrack) {
                existingTrack->setIndex(i);
                if (existingTrack->pad() == pad)
                    continue;
            }
        }

        auto track = VideoTrackPrivateGStreamer::create(makeWeakPtr(*this), i, pad);
        ASSERT(streamId == track->id());
        m_videoTracks.add(streamId, track.copyRef());
        m_player->addVideoTrack(track.get());
    }

    purgeInvalidVideoTracks(validVideoStreams);
#endif

    m_player->client().mediaPlayerEngineUpdated(m_player);
}

void MediaPlayerPrivateGStreamer::videoSinkCapsChangedCallback(MediaPlayerPrivateGStreamer* player)
{
    player->m_notifier->notify(MainThreadNotification::VideoCapsChanged, [player] {
        player->notifyPlayerOfVideoCaps();
    });
}

void MediaPlayerPrivateGStreamer::notifyPlayerOfVideoCaps()
{
    m_videoSize = IntSize();
    m_player->client().mediaPlayerEngineUpdated(m_player);
}

void MediaPlayerPrivateGStreamer::audioChangedCallback(MediaPlayerPrivateGStreamer* player)
{
    player->m_notifier->notify(MainThreadNotification::AudioChanged, [player] {
        player->notifyPlayerOfAudio();
    });
}

void MediaPlayerPrivateGStreamer::notifyPlayerOfAudio()
{
    if (UNLIKELY(!m_pipeline || !m_source))
        return;

    ASSERT(m_isLegacyPlaybin || isMediaSource());

    gint numTracks = 0;
    bool useMediaSource = isMediaSource();
    GstElement* element = useMediaSource ? m_source.get() : m_pipeline.get();
    g_object_get(element, "n-audio", &numTracks, nullptr);

    GST_INFO_OBJECT(pipeline(), "Media has %d audio tracks", numTracks);
    bool oldHasAudio = m_hasAudio;
    m_hasAudio = numTracks > 0;
    if (oldHasAudio != m_hasAudio)
        m_player->characteristicChanged();

    if (useMediaSource) {
        GST_DEBUG_OBJECT(pipeline(), "Tracks managed by source element. Bailing out now.");
        m_player->client().mediaPlayerEngineUpdated(m_player);
        return;
    }

#if ENABLE(VIDEO_TRACK)
    Vector<String> validAudioStreams;
    for (gint i = 0; i < numTracks; ++i) {
        GRefPtr<GstPad> pad;
        g_signal_emit_by_name(m_pipeline.get(), "get-audio-pad", i, &pad.outPtr(), nullptr);
        ASSERT(pad);

        String streamId = "A" + String::number(i);
        validAudioStreams.append(streamId);
        if (i < static_cast<gint>(m_audioTracks.size())) {
            RefPtr<AudioTrackPrivateGStreamer> existingTrack = m_audioTracks.get(streamId);
            if (existingTrack) {
                existingTrack->setIndex(i);
                if (existingTrack->pad() == pad)
                    continue;
            }
        }

        auto track = AudioTrackPrivateGStreamer::create(makeWeakPtr(*this), i, pad);
        ASSERT(streamId == track->id());
        m_audioTracks.add(streamId, track);
        m_player->addAudioTrack(*track);
    }

    purgeInvalidAudioTracks(validAudioStreams);
#endif

    m_player->client().mediaPlayerEngineUpdated(m_player);
}

#if ENABLE(VIDEO_TRACK)
void MediaPlayerPrivateGStreamer::textChangedCallback(MediaPlayerPrivateGStreamer* player)
{
    player->m_notifier->notify(MainThreadNotification::TextChanged, [player] {
        player->notifyPlayerOfText();
    });
}

void MediaPlayerPrivateGStreamer::notifyPlayerOfText()
{
    if (UNLIKELY(!m_pipeline || !m_source))
        return;

    ASSERT(m_isLegacyPlaybin || isMediaSource());

    gint numTracks = 0;
    bool useMediaSource = isMediaSource();
    GstElement* element = useMediaSource ? m_source.get() : m_pipeline.get();
    g_object_get(element, "n-text", &numTracks, nullptr);

    GST_INFO_OBJECT(pipeline(), "Media has %d text tracks", numTracks);

    if (useMediaSource) {
        GST_DEBUG_OBJECT(pipeline(), "Tracks managed by source element. Bailing out now.");
        return;
    }

    Vector<String> validTextStreams;
    for (gint i = 0; i < numTracks; ++i) {
        GRefPtr<GstPad> pad;
        g_signal_emit_by_name(m_pipeline.get(), "get-text-pad", i, &pad.outPtr(), nullptr);
        ASSERT(pad);

        // We can't assume the pad has a sticky event here like implemented in
        // InbandTextTrackPrivateGStreamer because it might be emitted after the
        // track was created. So fallback to a dummy stream ID like in the Audio
        // and Video tracks.
        String streamId = "T" + String::number(i);

        validTextStreams.append(streamId);
        if (i < static_cast<gint>(m_textTracks.size())) {
            RefPtr<InbandTextTrackPrivateGStreamer> existingTrack = m_textTracks.get(streamId);
            if (existingTrack) {
                existingTrack->setIndex(i);
                if (existingTrack->pad() == pad)
                    continue;
            }
        }

        auto track = InbandTextTrackPrivateGStreamer::create(i, pad);
        m_textTracks.add(streamId, track.copyRef());
        m_player->addTextTrack(track.get());
    }

    purgeInvalidTextTracks(validTextStreams);
}

GstFlowReturn MediaPlayerPrivateGStreamer::newTextSampleCallback(MediaPlayerPrivateGStreamer* player)
{
    player->newTextSample();
    return GST_FLOW_OK;
}

void MediaPlayerPrivateGStreamer::newTextSample()
{
    if (!m_textAppSink)
        return;

    GRefPtr<GstEvent> streamStartEvent = adoptGRef(
        gst_pad_get_sticky_event(m_textAppSinkPad.get(), GST_EVENT_STREAM_START, 0));

    GRefPtr<GstSample> sample;
    g_signal_emit_by_name(m_textAppSink.get(), "pull-sample", &sample.outPtr(), nullptr);
    ASSERT(sample);

    if (streamStartEvent) {
        bool found = FALSE;
        const gchar* id;
        gst_event_parse_stream_start(streamStartEvent.get(), &id);
        for (auto& track : m_textTracks.values()) {
            if (!strcmp(track->streamId().utf8().data(), id)) {
                track->handleSample(sample);
                found = true;
                break;
            }
        }
        if (!found)
            GST_WARNING("Got sample with unknown stream ID %s.", id);
    } else
        GST_WARNING("Unable to handle sample with no stream start event.");
}
#endif

void MediaPlayerPrivateGStreamer::fillTimerFired()
{
    if (m_errorOccured) {
        GST_DEBUG_OBJECT(pipeline(), "[Buffering] An error occurred, disabling the fill timer");
        m_fillTimer.stop();
        return;
    }

    GRefPtr<GstQuery> query = adoptGRef(gst_query_new_buffering(GST_FORMAT_PERCENT));
    double fillStatus = 100.0;
    GstBufferingMode mode = GST_BUFFERING_DOWNLOAD;

    if (gst_element_query(m_source.get(), query.get())) {
        gst_query_parse_buffering_stats(query.get(), &mode, nullptr, nullptr, nullptr);

        int percentage;
        gst_query_parse_buffering_percent(query.get(), nullptr, &percentage);
        fillStatus = percentage;
    } else if (m_httpResponseTotalSize) {
        GST_DEBUG_OBJECT(pipeline(), "[Buffering] Query failed, falling back to network read position estimation");
        fillStatus = 100.0 * (static_cast<double>(m_networkReadPosition) / static_cast<double>(m_httpResponseTotalSize));
    } else {
        GST_DEBUG_OBJECT(pipeline(), "[Buffering] Unable to determine on-disk buffering status");
        return;
    }

    updateBufferingStatus(mode, fillStatus);
}

void MediaPlayerPrivateGStreamer::loadStateChanged()
{
    updateStates();
}

void MediaPlayerPrivateGStreamer::timeChanged()
{
    updateStates();
    m_player->timeChanged();
}

void MediaPlayerPrivateGStreamer::loadingFailed(MediaPlayer::NetworkState networkError, MediaPlayer::ReadyState readyState, bool forceNotifications)
{
    GST_WARNING("Loading failed, error: %s", convertEnumerationToString(networkError).utf8().data());

    m_errorOccured = true;
    if (forceNotifications || m_networkState != networkError) {
        m_networkState = networkError;
        m_player->networkStateChanged();
    }
    if (forceNotifications || m_readyState != readyState) {
        m_readyState = readyState;
        m_player->readyStateChanged();
    }

    // Loading failed, remove ready timer.
    m_readyTimerHandler.stop();
}

GstElement* MediaPlayerPrivateGStreamer::createAudioSink()
{
    m_autoAudioSink = gst_element_factory_make("autoaudiosink", nullptr);
    if (!m_autoAudioSink) {
        GST_WARNING("GStreamer's autoaudiosink not found. Please check your gst-plugins-good installation");
        return nullptr;
    }

    g_signal_connect_swapped(m_autoAudioSink.get(), "child-added", G_CALLBACK(setAudioStreamPropertiesCallback), this);

#if ENABLE(WEB_AUDIO)
    GstElement* audioSinkBin = gst_bin_new("audio-sink");
    ensureAudioSourceProvider();
    m_audioSourceProvider->configureAudioBin(audioSinkBin, nullptr);
    return audioSinkBin;
#else
    return m_autoAudioSink.get();
#endif
}

GstElement* MediaPlayerPrivateGStreamer::audioSink() const
{
    GstElement* sink;
    g_object_get(m_pipeline.get(), "audio-sink", &sink, nullptr);
    return sink;
}

MediaTime MediaPlayerPrivateGStreamer::playbackPosition() const
{
    GST_TRACE_OBJECT(pipeline(), "isEndReached: %s, seeking: %s, seekTime: %s", boolForPrinting(m_isEndReached), boolForPrinting(m_seeking), m_seekTime.toString().utf8().data());
    if (m_isEndReached && m_seeking)
        return m_seekTime;

    // This constant should remain lower than HTMLMediaElement's maxTimeupdateEventFrequency.
    static const Seconds positionCacheThreshold = 200_ms;
    Seconds now = WTF::WallTime::now().secondsSinceEpoch();
    if (m_lastQueryTime && (now - m_lastQueryTime.value()) < positionCacheThreshold && m_cachedPosition.isValid()) {
        GST_TRACE_OBJECT(pipeline(), "Returning cached position: %s", m_cachedPosition.toString().utf8().data());
        return m_cachedPosition;
    }

    m_lastQueryTime = now;

    // Position is only available if no async state change is going on and the state is either paused or playing.
    gint64 position = GST_CLOCK_TIME_NONE;
    GstQuery* query = gst_query_new_position(GST_FORMAT_TIME);
    if (gst_element_query(m_pipeline.get(), query))
        gst_query_parse_position(query, 0, &position);
    gst_query_unref(query);

    GST_TRACE_OBJECT(pipeline(), "Position %" GST_TIME_FORMAT ", canFallBackToLastFinishedSeekPosition: %s", GST_TIME_ARGS(position), boolForPrinting(m_canFallBackToLastFinishedSeekPosition));

    MediaTime playbackPosition = MediaTime::zeroTime();
    GstClockTime gstreamerPosition = static_cast<GstClockTime>(position);
    if (GST_CLOCK_TIME_IS_VALID(gstreamerPosition))
        playbackPosition = MediaTime(gstreamerPosition, GST_SECOND);
    else if (m_canFallBackToLastFinishedSeekPosition)
        playbackPosition = m_seekTime;

    m_cachedPosition = playbackPosition;
    return playbackPosition;
}

void MediaPlayerPrivateGStreamer::enableTrack(TrackPrivateBaseGStreamer::TrackType trackType, unsigned index)
{
    // FIXME: Remove isMediaSource() test below when fixing https://bugs.webkit.org/show_bug.cgi?id=182531.
    if (isMediaSource()) {
        GST_FIXME_OBJECT(m_pipeline.get(), "Audio/Video/Text track switching is not yet supported by the MSE backend.");
        return;
    }

    const char* propertyName;
    const char* trackTypeAsString;
    Vector<String> selectedStreams;
    String selectedStreamId;

    GstStream* stream = nullptr;

    if (!m_isLegacyPlaybin) {
        stream = gst_stream_collection_get_stream(m_streamCollection.get(), index);
        if (!stream) {
            GST_WARNING_OBJECT(pipeline(), "No stream to select at index %u", index);
            return;
        }
        selectedStreamId = String::fromUTF8(gst_stream_get_stream_id(stream));
        selectedStreams.append(selectedStreamId);
    }

    switch (trackType) {
    case TrackPrivateBaseGStreamer::TrackType::Audio:
        propertyName = "current-audio";
        trackTypeAsString = "audio";
        if (!selectedStreamId.isEmpty() && selectedStreamId == m_currentAudioStreamId) {
            GST_INFO_OBJECT(pipeline(), "%s stream: %s already selected, not doing anything.", trackTypeAsString, selectedStreamId.utf8().data());
            return;
        }

        if (!m_currentTextStreamId.isEmpty())
            selectedStreams.append(m_currentTextStreamId);
        if (!m_currentVideoStreamId.isEmpty())
            selectedStreams.append(m_currentVideoStreamId);
        break;
    case TrackPrivateBaseGStreamer::TrackType::Video:
        propertyName = "current-video";
        trackTypeAsString = "video";
        if (!selectedStreamId.isEmpty() && selectedStreamId == m_currentVideoStreamId) {
            GST_INFO_OBJECT(pipeline(), "%s stream: %s already selected, not doing anything.", trackTypeAsString, selectedStreamId.utf8().data());
            return;
        }

        if (!m_currentAudioStreamId.isEmpty())
            selectedStreams.append(m_currentAudioStreamId);
        if (!m_currentTextStreamId.isEmpty())
            selectedStreams.append(m_currentTextStreamId);
        break;
    case TrackPrivateBaseGStreamer::TrackType::Text:
        propertyName = "current-text";
        trackTypeAsString = "text";
        if (!selectedStreamId.isEmpty() && selectedStreamId == m_currentTextStreamId) {
            GST_INFO_OBJECT(pipeline(), "%s stream: %s already selected, not doing anything.", trackTypeAsString, selectedStreamId.utf8().data());
            return;
        }

        if (!m_currentAudioStreamId.isEmpty())
            selectedStreams.append(m_currentAudioStreamId);
        if (!m_currentVideoStreamId.isEmpty())
            selectedStreams.append(m_currentVideoStreamId);
        break;
    case TrackPrivateBaseGStreamer::TrackType::Unknown:
    default:
        ASSERT_NOT_REACHED();
    }

    GST_INFO_OBJECT(pipeline(), "Enabling %s track with index: %u", trackTypeAsString, index);
    if (m_isLegacyPlaybin)
        g_object_set(m_pipeline.get(), propertyName, index, nullptr);
    else {
        GList* selectedStreamsList = nullptr;

        for (const auto& streamId : selectedStreams)
            selectedStreamsList = g_list_append(selectedStreamsList, g_strdup(streamId.utf8().data()));

        // TODO: MSE GstStream API support: https://bugs.webkit.org/show_bug.cgi?id=182531
        gst_element_send_event(m_pipeline.get(), gst_event_new_select_streams(selectedStreamsList));
        g_list_free_full(selectedStreamsList, reinterpret_cast<GDestroyNotify>(g_free));
    }
}

void MediaPlayerPrivateGStreamer::updateTracks()
{
    ASSERT(!m_isLegacyPlaybin);

    bool useMediaSource = isMediaSource();
    unsigned length = gst_stream_collection_get_size(m_streamCollection.get());

    bool oldHasAudio = m_hasAudio;
    bool oldHasVideo = m_hasVideo;
    // New stream collections override previous ones.
    clearTracks();
    unsigned textTrackIndex = 0;
    for (unsigned i = 0; i < length; i++) {
        GRefPtr<GstStream> stream = gst_stream_collection_get_stream(m_streamCollection.get(), i);
        String streamId(gst_stream_get_stream_id(stream.get()));
        GstStreamType type = gst_stream_get_stream_type(stream.get());

        GST_DEBUG_OBJECT(pipeline(), "Inspecting %s track with ID %s", gst_stream_type_get_name(type), streamId.utf8().data());
        if (type & GST_STREAM_TYPE_AUDIO)
            CREATE_TRACK(audio, Audio);
        else if (type & GST_STREAM_TYPE_VIDEO)
            CREATE_TRACK(video, Video);
        else if (type & GST_STREAM_TYPE_TEXT && !useMediaSource) {
#if ENABLE(VIDEO_TRACK)
            auto track = InbandTextTrackPrivateGStreamer::create(textTrackIndex++, stream);
            m_textTracks.add(streamId, track.copyRef());
            m_player->addTextTrack(track.get());
#endif
        } else
            GST_WARNING("Unknown track type found for stream %s", streamId.utf8().data());
    }

    if ((oldHasVideo != m_hasVideo) || (oldHasAudio != m_hasAudio))
        m_player->characteristicChanged();

    if (m_hasVideo)
        m_player->sizeChanged();

    m_player->client().mediaPlayerEngineUpdated(m_player);
}

void MediaPlayerPrivateGStreamer::clearTracks()
{
#if ENABLE(VIDEO_TRACK)
    CLEAR_TRACKS(m_audioTracks, m_player->removeAudioTrack);
    CLEAR_TRACKS(m_videoTracks, m_player->removeVideoTrack);
    CLEAR_TRACKS(m_textTracks, m_player->removeTextTrack);
#endif // ENABLE(VIDEO_TRACK)
}

void MediaPlayerPrivateGStreamer::videoChangedCallback(MediaPlayerPrivateGStreamer* player)
{
    player->m_notifier->notify(MainThreadNotification::VideoChanged, [player] {
        player->notifyPlayerOfVideo();
    });
}

void MediaPlayerPrivateGStreamer::setPipeline(GstElement* pipeline)
{
    m_pipeline = pipeline;

    GRefPtr<GstBus> bus = adoptGRef(gst_pipeline_get_bus(GST_PIPELINE(m_pipeline.get())));
    gst_bus_set_sync_handler(bus.get(), [](GstBus*, GstMessage* message, gpointer userData) {
        auto& player = *static_cast<MediaPlayerPrivateGStreamer*>(userData);

        if (player.handleSyncMessage(message)) {
            gst_message_unref(message);
            return GST_BUS_DROP;
        }

        return GST_BUS_PASS;
    }, this, nullptr);
}

bool MediaPlayerPrivateGStreamer::handleSyncMessage(GstMessage* message)
{
    if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_STREAM_COLLECTION && !m_isLegacyPlaybin) {
        GRefPtr<GstStreamCollection> collection;
        gst_message_parse_stream_collection(message, &collection.outPtr());

        if (collection) {
            m_streamCollection.swap(collection);
            m_notifier->notify(MainThreadNotification::StreamCollectionChanged, [this] {
                this->updateTracks();
            });
        }
    }

    if (GST_MESSAGE_TYPE(message) != GST_MESSAGE_NEED_CONTEXT)
        return false;

    const gchar* contextType;
    gst_message_parse_context_type(message, &contextType);
    GST_DEBUG_OBJECT(pipeline(), "Handling %s need-context message for %s", contextType, GST_MESSAGE_SRC_NAME(message));

    if (!g_strcmp0(contextType, WEBKIT_WEB_SRC_PLAYER_CONTEXT_TYPE_NAME)) {
        GRefPtr<GstContext> context = adoptGRef(gst_context_new(WEBKIT_WEB_SRC_PLAYER_CONTEXT_TYPE_NAME, FALSE));
        GstStructure* contextStructure = gst_context_writable_structure(context.get());

        ASSERT(m_player);
        gst_structure_set(contextStructure, "player", G_TYPE_POINTER, m_player, nullptr);
        gst_element_set_context(GST_ELEMENT(GST_MESSAGE_SRC(message)), context.get());
        return true;
    }

#if USE(GSTREAMER_GL)
    GRefPtr<GstContext> elementContext = adoptGRef(requestGLContext(contextType));
    if (elementContext) {
        gst_element_set_context(GST_ELEMENT(message->src), elementContext.get());
        return true;
    }
#endif // USE(GSTREAMER_GL)

#if ENABLE(ENCRYPTED_MEDIA)
    if (!g_strcmp0(contextType, "drm-preferred-decryption-system-id")) {
        if (isMainThread()) {
            GST_ERROR("can't handle drm-preferred-decryption-system-id need context message in the main thread");
            ASSERT_NOT_REACHED();
            return false;
        }
        GST_DEBUG_OBJECT(pipeline(), "handling drm-preferred-decryption-system-id need context message");

        InitData initData;
        {
            LockHolder lock(m_protectionMutex);
            ProtectionSystemEvents protectionSystemEvents(message);
            GST_TRACE("found %zu protection events, %zu decryptors available", protectionSystemEvents.events().size(), protectionSystemEvents.availableSystems().size());

            for (auto& event : protectionSystemEvents.events()) {
                const char* eventKeySystemId = nullptr;
                GstBuffer* data = nullptr;
                gst_event_parse_protection(event.get(), &eventKeySystemId, &data, nullptr);

                initData.append({eventKeySystemId, data});
                m_handledProtectionEvents.add(GST_EVENT_SEQNUM(event.get()));
            }
        }
        initializationDataEncountered(WTFMove(initData));

        GST_INFO_OBJECT(pipeline(), "waiting for a CDM instance");
        if (m_cdmAttachmentSemaphore.waitFor(4_s)
            && m_notifier->isValid() // Check the player is not being destroyed.
            && !m_cdmInstance->keySystem().isEmpty()) {
            const char* preferredKeySystemUuid = GStreamerEMEUtilities::keySystemToUuid(m_cdmInstance->keySystem());
            GST_INFO_OBJECT(pipeline(), "working with key system %s, continuing with key system %s on %s", m_cdmInstance->keySystem().utf8().data(), preferredKeySystemUuid, GST_MESSAGE_SRC_NAME(message));

            GRefPtr<GstContext> context = adoptGRef(gst_context_new("drm-preferred-decryption-system-id", FALSE));
            GstStructure* contextStructure = gst_context_writable_structure(context.get());
            gst_structure_set(contextStructure, "decryption-system-id", G_TYPE_STRING, preferredKeySystemUuid, nullptr);
            gst_element_set_context(GST_ELEMENT(GST_MESSAGE_SRC(message)), context.get());
        } else
            GST_WARNING("CDM instance not initializaed");

        return true;
    }
#endif // ENABLE(ENCRYPTED_MEDIA)
    return false;
}

#if USE(GSTREAMER_GL)
GstContext* MediaPlayerPrivateGStreamer::requestGLContext(const char* contextType)
{
    if (!ensureGstGLContext())
        return nullptr;

    if (!g_strcmp0(contextType, GST_GL_DISPLAY_CONTEXT_TYPE)) {
        GstContext* displayContext = gst_context_new(GST_GL_DISPLAY_CONTEXT_TYPE, TRUE);
        gst_context_set_gl_display(displayContext, gstGLDisplay());
        return displayContext;
    }

    if (!g_strcmp0(contextType, "gst.gl.app_context")) {
        GstContext* appContext = gst_context_new("gst.gl.app_context", TRUE);
        GstStructure* structure = gst_context_writable_structure(appContext);
#if GST_CHECK_VERSION(1, 12, 0)
        gst_structure_set(structure, "context", GST_TYPE_GL_CONTEXT, gstGLContext(), nullptr);
#else
        gst_structure_set(structure, "context", GST_GL_TYPE_CONTEXT, gstGLContext(), nullptr);
#endif
        return appContext;
    }

    return nullptr;
}

bool MediaPlayerPrivateGStreamer::ensureGstGLContext()
{
    if (m_glContext)
        return true;

    auto& sharedDisplay = PlatformDisplay::sharedDisplayForCompositing();

    // The floating ref removal support was added in https://bugzilla.gnome.org/show_bug.cgi?id=743062.
    bool shouldAdoptRef = webkitGstCheckVersion(1, 14, 0);
    if (!m_glDisplay) {
#if PLATFORM(X11)
#if USE(GLX)
        if (is<PlatformDisplayX11>(sharedDisplay)) {
            GST_DEBUG_OBJECT(pipeline(), "Creating X11 shared GL display");
            if (shouldAdoptRef)
                m_glDisplay = adoptGRef(GST_GL_DISPLAY(gst_gl_display_x11_new_with_display(downcast<PlatformDisplayX11>(sharedDisplay).native())));
            else
                m_glDisplay = GST_GL_DISPLAY(gst_gl_display_x11_new_with_display(downcast<PlatformDisplayX11>(sharedDisplay).native()));
        }
#elif USE(EGL)
        if (is<PlatformDisplayX11>(sharedDisplay)) {
            GST_DEBUG_OBJECT(pipeline(), "Creating X11 shared EGL display");
            if (shouldAdoptRef)
                m_glDisplay = adoptGRef(GST_GL_DISPLAY(gst_gl_display_egl_new_with_egl_display(downcast<PlatformDisplayX11>(sharedDisplay).eglDisplay())));
            else
                m_glDisplay = GST_GL_DISPLAY(gst_gl_display_egl_new_with_egl_display(downcast<PlatformDisplayX11>(sharedDisplay).eglDisplay()));
        }
#endif
#endif

#if PLATFORM(WAYLAND)
        if (is<PlatformDisplayWayland>(sharedDisplay)) {
            GST_DEBUG_OBJECT(pipeline(), "Creating Wayland shared display");
            if (shouldAdoptRef)
                m_glDisplay = adoptGRef(GST_GL_DISPLAY(gst_gl_display_egl_new_with_egl_display(downcast<PlatformDisplayWayland>(sharedDisplay).eglDisplay())));
            else
                m_glDisplay = GST_GL_DISPLAY(gst_gl_display_egl_new_with_egl_display(downcast<PlatformDisplayWayland>(sharedDisplay).eglDisplay()));
        }
#endif

#if USE(WPE_RENDERER)
        if (is<PlatformDisplayLibWPE>(sharedDisplay)) {
            GST_DEBUG_OBJECT(pipeline(), "Creating WPE shared EGL display");
            if (shouldAdoptRef)
                m_glDisplay = adoptGRef(GST_GL_DISPLAY(gst_gl_display_egl_new_with_egl_display(downcast<PlatformDisplayLibWPE>(sharedDisplay).eglDisplay())));
            else
                m_glDisplay = GST_GL_DISPLAY(gst_gl_display_egl_new_with_egl_display(downcast<PlatformDisplayLibWPE>(sharedDisplay).eglDisplay()));
        }
#endif

        ASSERT(m_glDisplay);
    }

    GLContext* webkitContext = sharedDisplay.sharingGLContext();
    // EGL and GLX are mutually exclusive, no need for ifdefs here.
    GstGLPlatform glPlatform = webkitContext->isEGLContext() ? GST_GL_PLATFORM_EGL : GST_GL_PLATFORM_GLX;

#if USE(OPENGL_ES)
    GstGLAPI glAPI = GST_GL_API_GLES2;
#elif USE(OPENGL)
    GstGLAPI glAPI = GST_GL_API_OPENGL;
#else
    ASSERT_NOT_REACHED();
#endif

    PlatformGraphicsContext3D contextHandle = webkitContext->platformContext();
    if (!contextHandle)
        return false;

    if (shouldAdoptRef)
        m_glContext = adoptGRef(gst_gl_context_new_wrapped(m_glDisplay.get(), reinterpret_cast<guintptr>(contextHandle), glPlatform, glAPI));
    else
        m_glContext = gst_gl_context_new_wrapped(m_glDisplay.get(), reinterpret_cast<guintptr>(contextHandle), glPlatform, glAPI);

    // Activate and fill the GStreamer wrapped context with the Webkit's shared one.
    auto previousActiveContext = GLContext::current();
    webkitContext->makeContextCurrent();
    if (gst_gl_context_activate(m_glContext.get(), TRUE)) {
        GUniqueOutPtr<GError> error;
        if (!gst_gl_context_fill_info(m_glContext.get(), &error.outPtr()))
            GST_WARNING("Failed to fill in GStreamer context: %s", error->message);
        gst_gl_context_activate(m_glContext.get(), FALSE);
    } else
        GST_WARNING("Failed to activate GStreamer context %" GST_PTR_FORMAT, m_glContext.get());
    if (previousActiveContext)
        previousActiveContext->makeContextCurrent();

    return true;
}
#endif // USE(GSTREAMER_GL)

// Returns the size of the video
FloatSize MediaPlayerPrivateGStreamer::naturalSize() const
{
#if USE(GSTREAMER_HOLEPUNCH)
    // When using the holepuch we may not be able to get the video frames size, so we can't use
    // it. But we need to report some non empty naturalSize for the player's GraphicsLayer
    // to be properly created.
    return s_holePunchDefaultFrameSize;
#endif

#if ENABLE(MEDIA_STREAM)
    if (!m_isLegacyPlaybin && !m_currentVideoStreamId.isEmpty()) {
        RefPtr<VideoTrackPrivateGStreamer> videoTrack = m_videoTracks.get(m_currentVideoStreamId);

        if (videoTrack) {
            auto tags = adoptGRef(gst_stream_get_tags(videoTrack->stream()));
            gint width, height;

            if (tags && gst_tag_list_get_int(tags.get(), WEBKIT_MEDIA_TRACK_TAG_WIDTH, &width) && gst_tag_list_get_int(tags.get(), WEBKIT_MEDIA_TRACK_TAG_HEIGHT, &height))
                return FloatSize(width, height);
        }
    }
#endif // ENABLE(MEDIA_STREAM)

    if (!hasVideo())
        return FloatSize();

    if (!m_videoSize.isEmpty())
        return m_videoSize;

    auto sampleLocker = holdLock(m_sampleMutex);
    if (!GST_IS_SAMPLE(m_sample.get()))
        return FloatSize();

    GstCaps* caps = gst_sample_get_caps(m_sample.get());
    if (!caps)
        return FloatSize();

    // TODO: handle possible clean aperture data. See
    // https://bugzilla.gnome.org/show_bug.cgi?id=596571
    // TODO: handle possible transformation matrix. See
    // https://bugzilla.gnome.org/show_bug.cgi?id=596326

    // Get the video PAR and original size, if this fails the
    // video-sink has likely not yet negotiated its caps.
    int pixelAspectRatioNumerator, pixelAspectRatioDenominator, stride;
    IntSize originalSize;
    GstVideoFormat format;
    if (!getVideoSizeAndFormatFromCaps(caps, originalSize, format, pixelAspectRatioNumerator, pixelAspectRatioDenominator, stride))
        return FloatSize();

#if USE(TEXTURE_MAPPER_GL)
    // When using accelerated compositing, if the video is tagged as rotated 90 or 270 degrees, swap width and height.
    if (m_renderingCanBeAccelerated) {
        if (m_videoSourceOrientation.usesWidthAsHeight())
            originalSize = originalSize.transposedSize();
    }
#endif

    GST_DEBUG_OBJECT(pipeline(), "Original video size: %dx%d", originalSize.width(), originalSize.height());
    GST_DEBUG_OBJECT(pipeline(), "Pixel aspect ratio: %d/%d", pixelAspectRatioNumerator, pixelAspectRatioDenominator);

    // Calculate DAR based on PAR and video size.
    int displayWidth = originalSize.width() * pixelAspectRatioNumerator;
    int displayHeight = originalSize.height() * pixelAspectRatioDenominator;

    // Divide display width and height by their GCD to avoid possible overflows.
    int displayAspectRatioGCD = greatestCommonDivisor(displayWidth, displayHeight);
    displayWidth /= displayAspectRatioGCD;
    displayHeight /= displayAspectRatioGCD;

    // Apply DAR to original video size. This is the same behavior as in xvimagesink's setcaps function.
    guint64 width = 0, height = 0;
    if (!(originalSize.height() % displayHeight)) {
        GST_DEBUG_OBJECT(pipeline(), "Keeping video original height");
        width = gst_util_uint64_scale_int(originalSize.height(), displayWidth, displayHeight);
        height = static_cast<guint64>(originalSize.height());
    } else if (!(originalSize.width() % displayWidth)) {
        GST_DEBUG_OBJECT(pipeline(), "Keeping video original width");
        height = gst_util_uint64_scale_int(originalSize.width(), displayHeight, displayWidth);
        width = static_cast<guint64>(originalSize.width());
    } else {
        GST_DEBUG_OBJECT(pipeline(), "Approximating while keeping original video height");
        width = gst_util_uint64_scale_int(originalSize.height(), displayWidth, displayHeight);
        height = static_cast<guint64>(originalSize.height());
    }

    GST_DEBUG_OBJECT(pipeline(), "Natural size: %" G_GUINT64_FORMAT "x%" G_GUINT64_FORMAT, width, height);
    m_videoSize = FloatSize(static_cast<int>(width), static_cast<int>(height));
    return m_videoSize;
}

void MediaPlayerPrivateGStreamer::setVolume(float volume)
{
    if (!m_volumeElement)
        return;

    GST_DEBUG_OBJECT(pipeline(), "Setting volume: %f", volume);
    gst_stream_volume_set_volume(m_volumeElement.get(), GST_STREAM_VOLUME_FORMAT_LINEAR, static_cast<double>(volume));
}

float MediaPlayerPrivateGStreamer::volume() const
{
    if (!m_volumeElement)
        return 0;

    return gst_stream_volume_get_volume(m_volumeElement.get(), GST_STREAM_VOLUME_FORMAT_LINEAR);
}

void MediaPlayerPrivateGStreamer::notifyPlayerOfVolumeChange()
{
    if (!m_player || !m_volumeElement)
        return;
    double volume;
    volume = gst_stream_volume_get_volume(m_volumeElement.get(), GST_STREAM_VOLUME_FORMAT_LINEAR);
    // get_volume() can return values superior to 1.0 if the user
    // applies software user gain via third party application (GNOME
    // volume control for instance).
    volume = CLAMP(volume, 0.0, 1.0);
    m_player->volumeChanged(static_cast<float>(volume));
}

void MediaPlayerPrivateGStreamer::volumeChangedCallback(MediaPlayerPrivateGStreamer* player)
{
    // This is called when m_volumeElement receives the notify::volume signal.
    GST_DEBUG_OBJECT(player->pipeline(), "Volume changed to: %f", player->volume());

    player->m_notifier->notify(MainThreadNotification::VolumeChanged, [player] {
        player->notifyPlayerOfVolumeChange();
    });
}

MediaPlayer::NetworkState MediaPlayerPrivateGStreamer::networkState() const
{
    return m_networkState;
}

MediaPlayer::ReadyState MediaPlayerPrivateGStreamer::readyState() const
{
    return m_readyState;
}

void MediaPlayerPrivateGStreamer::sizeChanged()
{
    notImplemented();
}

void MediaPlayerPrivateGStreamer::setMuted(bool mute)
{
    if (!m_volumeElement)
        return;

    bool currentValue = muted();
    if (currentValue == mute)
        return;

    GST_INFO_OBJECT(pipeline(), "Set muted to %s", toString(mute).utf8().data());
    g_object_set(m_volumeElement.get(), "mute", mute, nullptr);
}

bool MediaPlayerPrivateGStreamer::muted() const
{
    if (!m_volumeElement)
        return false;

    gboolean muted;
    g_object_get(m_volumeElement.get(), "mute", &muted, nullptr);
    GST_INFO_OBJECT(pipeline(), "Player is muted: %s", toString(static_cast<bool>(muted)).utf8().data());
    return muted;
}

void MediaPlayerPrivateGStreamer::notifyPlayerOfMute()
{
    if (!m_player || !m_volumeElement)
        return;

    gboolean muted;
    g_object_get(m_volumeElement.get(), "mute", &muted, nullptr);
    m_player->muteChanged(static_cast<bool>(muted));
}

void MediaPlayerPrivateGStreamer::muteChangedCallback(MediaPlayerPrivateGStreamer* player)
{
    // This is called when m_volumeElement receives the notify::mute signal.
    player->m_notifier->notify(MainThreadNotification::MuteChanged, [player] {
        player->notifyPlayerOfMute();
    });
}

void MediaPlayerPrivateGStreamer::handleMessage(GstMessage* message)
{
    GUniqueOutPtr<GError> err;
    GUniqueOutPtr<gchar> debug;
    MediaPlayer::NetworkState error;
    bool issueError = true;
    bool attemptNextLocation = false;
    const GstStructure* structure = gst_message_get_structure(message);
    GstState requestedState, currentState;

    m_canFallBackToLastFinishedSeekPosition = false;

    if (structure) {
        const gchar* messageTypeName = gst_structure_get_name(structure);

        // Redirect messages are sent from elements, like qtdemux, to
        // notify of the new location(s) of the media.
        if (!g_strcmp0(messageTypeName, "redirect")) {
            mediaLocationChanged(message);
            return;
        }
    }

    // We ignore state changes from internal elements. They are forwarded to playbin2 anyway.
    bool messageSourceIsPlaybin = GST_MESSAGE_SRC(message) == reinterpret_cast<GstObject*>(m_pipeline.get());

    GST_LOG_OBJECT(pipeline(), "Message %s received from element %s", GST_MESSAGE_TYPE_NAME(message), GST_MESSAGE_SRC_NAME(message));
    switch (GST_MESSAGE_TYPE(message)) {
    case GST_MESSAGE_ERROR:
        if (m_resetPipeline || !m_missingPluginCallbacks.isEmpty() || m_errorOccured)
            break;
        gst_message_parse_error(message, &err.outPtr(), &debug.outPtr());
        GST_ERROR("Error %d: %s (url=%s)", err->code, err->message, m_url.string().utf8().data());

        GST_DEBUG_BIN_TO_DOT_FILE_WITH_TS(GST_BIN(m_pipeline.get()), GST_DEBUG_GRAPH_SHOW_ALL, "webkit-video.error");

        error = MediaPlayer::Empty;
        if (g_error_matches(err.get(), GST_STREAM_ERROR, GST_STREAM_ERROR_CODEC_NOT_FOUND)
            || g_error_matches(err.get(), GST_STREAM_ERROR, GST_STREAM_ERROR_WRONG_TYPE)
            || g_error_matches(err.get(), GST_STREAM_ERROR, GST_STREAM_ERROR_FAILED)
            || g_error_matches(err.get(), GST_CORE_ERROR, GST_CORE_ERROR_MISSING_PLUGIN)
            || g_error_matches(err.get(), GST_RESOURCE_ERROR, GST_RESOURCE_ERROR_NOT_FOUND))
            error = MediaPlayer::FormatError;
        else if (g_error_matches(err.get(), GST_STREAM_ERROR, GST_STREAM_ERROR_TYPE_NOT_FOUND)) {
            // Let the mediaPlayerClient handle the stream error, in
            // this case the HTMLMediaElement will emit a stalled
            // event.
            GST_ERROR("Decode error, let the Media element emit a stalled event.");
            m_loadingStalled = true;
            break;
        } else if (err->domain == GST_STREAM_ERROR) {
            error = MediaPlayer::DecodeError;
            attemptNextLocation = true;
        } else if (err->domain == GST_RESOURCE_ERROR)
            error = MediaPlayer::NetworkError;

        if (attemptNextLocation)
            issueError = !loadNextLocation();
        if (issueError) {
            m_errorOccured = true;
            if (m_networkState != error) {
                m_networkState = error;
                m_player->networkStateChanged();
            }
        }
        break;
    case GST_MESSAGE_EOS:
        didEnd();
        break;
    case GST_MESSAGE_ASYNC_DONE:
        if (!messageSourceIsPlaybin || m_delayingLoad)
            break;
        asyncStateChangeDone();
        break;
    case GST_MESSAGE_STATE_CHANGED: {
        if (!messageSourceIsPlaybin || m_delayingLoad)
            break;
        updateStates();

        // Construct a filename for the graphviz dot file output.
        GstState newState;
        gst_message_parse_state_changed(message, &currentState, &newState, nullptr);
        CString dotFileName = makeString(GST_OBJECT_NAME(m_pipeline.get()), '.',
            gst_element_state_get_name(currentState), '_', gst_element_state_get_name(newState)).utf8();
        GST_DEBUG_BIN_TO_DOT_FILE_WITH_TS(GST_BIN(m_pipeline.get()), GST_DEBUG_GRAPH_SHOW_ALL, dotFileName.data());

        break;
    }
    case GST_MESSAGE_BUFFERING:
        processBufferingStats(message);
        break;
    case GST_MESSAGE_DURATION_CHANGED:
        // Duration in MSE is managed by MediaSource, SourceBuffer and AppendPipeline.
        if (messageSourceIsPlaybin && !isMediaSource())
            durationChanged();
        break;
    case GST_MESSAGE_REQUEST_STATE:
        gst_message_parse_request_state(message, &requestedState);
        gst_element_get_state(m_pipeline.get(), &currentState, nullptr, 250 * GST_NSECOND);
        if (requestedState < currentState) {
            GST_INFO_OBJECT(pipeline(), "Element %s requested state change to %s", GST_MESSAGE_SRC_NAME(message),
                gst_element_state_get_name(requestedState));
            m_requestedState = requestedState;
            if (!changePipelineState(requestedState))
                loadingFailed(MediaPlayer::Empty);
        }
        break;
    case GST_MESSAGE_CLOCK_LOST:
        // This can only happen in PLAYING state and we should just
        // get a new clock by moving back to PAUSED and then to
        // PLAYING again.
        // This can happen if the stream that ends in a sink that
        // provides the current clock disappears, for example if
        // the audio sink provides the clock and the audio stream
        // is disabled. It also happens relatively often with
        // HTTP adaptive streams when switching between different
        // variants of a stream.
        gst_element_set_state(m_pipeline.get(), GST_STATE_PAUSED);
        gst_element_set_state(m_pipeline.get(), GST_STATE_PLAYING);
        break;
    case GST_MESSAGE_LATENCY:
        // Recalculate the latency, we don't need any special handling
        // here other than the GStreamer default.
        // This can happen if the latency of live elements changes, or
        // for one reason or another a new live element is added or
        // removed from the pipeline.
        gst_bin_recalculate_latency(GST_BIN(m_pipeline.get()));
        break;
    case GST_MESSAGE_ELEMENT:
        if (gst_is_missing_plugin_message(message)) {
            if (gst_install_plugins_supported()) {
                auto missingPluginCallback = MediaPlayerRequestInstallMissingPluginsCallback::create([weakThis = makeWeakPtr(*this)](uint32_t result, MediaPlayerRequestInstallMissingPluginsCallback& missingPluginCallback) {
                    if (!weakThis) {
                        GST_INFO("got missing pluging installation callback in destroyed player with result %u", result);
                        return;
                    }

                    GST_DEBUG("got missing plugin installation callback with result %u", result);
                    RefPtr<MediaPlayerRequestInstallMissingPluginsCallback> protectedMissingPluginCallback = &missingPluginCallback;
                    weakThis->m_missingPluginCallbacks.removeFirst(protectedMissingPluginCallback);
                    if (result != GST_INSTALL_PLUGINS_SUCCESS)
                        return;

                    weakThis->changePipelineState(GST_STATE_READY);
                    weakThis->changePipelineState(GST_STATE_PAUSED);
                });
                m_missingPluginCallbacks.append(missingPluginCallback.copyRef());
                GUniquePtr<char> detail(gst_missing_plugin_message_get_installer_detail(message));
                GUniquePtr<char> description(gst_missing_plugin_message_get_description(message));
                m_player->client().requestInstallMissingPlugins(String::fromUTF8(detail.get()), String::fromUTF8(description.get()), missingPluginCallback.get());
            }
        }
#if ENABLE(VIDEO_TRACK) && USE(GSTREAMER_MPEGTS)
        else if (GstMpegtsSection* section = gst_message_parse_mpegts_section(message)) {
            processMpegTsSection(section);
            gst_mpegts_section_unref(section);
        }
#endif
#if ENABLE(ENCRYPTED_MEDIA)
        else if (gst_structure_has_name(structure, "drm-waiting-for-key")) {
            GST_DEBUG_OBJECT(pipeline(), "drm-waiting-for-key message from %s", GST_MESSAGE_SRC_NAME(message));
            setWaitingForKey(true);
            // FIXME: The decryptors should be able to attempt to decrypt after being created and linked in a pipeline but currently they are not and current
            // architecture does not make this very easy. Fortunately, the arch will change soon and it does not pay off to fix this now with something that could be
            // more convoluted. In the meantime, force attempt to decrypt when they get blocked.
            attemptToDecryptWithLocalInstance();
        } else if (gst_structure_has_name(structure, "drm-key-received")) {
            GST_DEBUG_OBJECT(pipeline(), "drm-key-received message from %s", GST_MESSAGE_SRC_NAME(message));
            setWaitingForKey(false);
        }
#endif
        else if (gst_structure_has_name(structure, "http-headers")) {
            GST_DEBUG_OBJECT(pipeline(), "Processing HTTP headers: %" GST_PTR_FORMAT, structure);
            if (const char* uri = gst_structure_get_string(structure, "uri")) {
                URL url(URL(), uri);
                convertToInternalProtocol(url);
                m_origins.add(SecurityOrigin::create(url));

                if (url != m_url) {
                    GST_DEBUG_OBJECT(pipeline(), "Ignoring HTTP response headers for non-main URI.");
                    break;
                }
            }

            bool isRangeRequest = false;
            GUniqueOutPtr<GstStructure> requestHeaders;
            if (gst_structure_get(structure, "request-headers", GST_TYPE_STRUCTURE, &requestHeaders.outPtr(), nullptr))
                isRangeRequest = gst_structure_has_field(requestHeaders.get(), "Range");

            GST_DEBUG_OBJECT(pipeline(), "Is range request: %s", boolForPrinting(isRangeRequest));

            GUniqueOutPtr<GstStructure> responseHeaders;
            if (gst_structure_get(structure, "response-headers", GST_TYPE_STRUCTURE, &responseHeaders.outPtr(), nullptr)) {
                CString contentLengthHeaderName = httpHeaderNameString(HTTPHeaderName::ContentLength).utf8();
                uint64_t contentLength = 0;
                if (!gst_structure_get_uint64(responseHeaders.get(), contentLengthHeaderName.data(), &contentLength)) {
                    // souphttpsrc sets a string for Content-Length, so
                    // handle it here, until we remove the webkit+ protocol
                    // prefix from webkitwebsrc.
                    if (const char* contentLengthAsString = gst_structure_get_string(responseHeaders.get(), contentLengthHeaderName.data())) {
                        contentLength = g_ascii_strtoull(contentLengthAsString, nullptr, 10);
                        if (contentLength == G_MAXUINT64)
                            contentLength = 0;
                    }
                }
                if (!isRangeRequest) {
                    m_isStreaming = !contentLength;
                    GST_INFO_OBJECT(pipeline(), "%s stream detected", m_isStreaming ? "Live" : "Non-live");
                    updateDownloadBufferingFlag();
                }
            }
        } else if (gst_structure_has_name(structure, "webkit-network-statistics")) {
            if (gst_structure_get(structure, "read-position", G_TYPE_UINT64, &m_networkReadPosition, "size", G_TYPE_UINT64, &m_httpResponseTotalSize, nullptr))
                GST_DEBUG_OBJECT(pipeline(), "Updated network read position %" G_GUINT64_FORMAT ", size: %" G_GUINT64_FORMAT, m_networkReadPosition, m_httpResponseTotalSize);
        } else if (gst_structure_has_name(structure, "adaptive-streaming-statistics")) {
            if (WEBKIT_IS_WEB_SRC(m_source.get()) && !webkitGstCheckVersion(1, 12, 0)) {
                if (const char* uri = gst_structure_get_string(structure, "uri"))
                    m_hasTaintedOrigin = webKitSrcWouldTaintOrigin(WEBKIT_WEB_SRC_CAST(m_source.get()), SecurityOrigin::create(URL(URL(), uri)));
            }
        } else
            GST_DEBUG_OBJECT(pipeline(), "Unhandled element message: %" GST_PTR_FORMAT, structure);
        break;
#if ENABLE(VIDEO_TRACK)
    case GST_MESSAGE_TOC:
        processTableOfContents(message);
        break;
#endif
    case GST_MESSAGE_TAG: {
        GstTagList* tags = nullptr;
        GUniqueOutPtr<gchar> tag;
        gst_message_parse_tag(message, &tags);
        if (gst_tag_list_get_string(tags, GST_TAG_IMAGE_ORIENTATION, &tag.outPtr())) {
            if (!g_strcmp0(tag.get(), "rotate-90"))
                setVideoSourceOrientation(ImageOrientation::OriginRightTop);
            else if (!g_strcmp0(tag.get(), "rotate-180"))
                setVideoSourceOrientation(ImageOrientation::OriginBottomRight);
            else if (!g_strcmp0(tag.get(), "rotate-270"))
                setVideoSourceOrientation(ImageOrientation::OriginLeftBottom);
        }
        gst_tag_list_unref(tags);
        break;
    }
    case GST_MESSAGE_STREAMS_SELECTED: {
        GRefPtr<GstStreamCollection> collection;
        gst_message_parse_streams_selected(message, &collection.outPtr());

        if (!collection)
            break;

        m_streamCollection.swap(collection);
        m_currentAudioStreamId = "";
        m_currentVideoStreamId = "";
        m_currentTextStreamId = "";

        unsigned length = gst_message_streams_selected_get_size(message);
        for (unsigned i = 0; i < length; i++) {
            GRefPtr<GstStream> stream = gst_message_streams_selected_get_stream(message, i);
            if (!stream)
                continue;

            GstStreamType type = gst_stream_get_stream_type(stream.get());
            String streamId(gst_stream_get_stream_id(stream.get()));

            GST_DEBUG_OBJECT(pipeline(), "Selecting %s track with ID: %s", gst_stream_type_get_name(type), streamId.utf8().data());
            // Playbin3 can send more than one selected stream of the same type
            // but there's no priority or ordering system in place, so we assume
            // the selected stream is the last one as reported by playbin3.
            if (type & GST_STREAM_TYPE_AUDIO) {
                m_currentAudioStreamId = streamId;
                auto track = m_audioTracks.get(m_currentAudioStreamId);
                ASSERT(track);
                track->markAsActive();
            } else if (type & GST_STREAM_TYPE_VIDEO) {
                m_currentVideoStreamId = streamId;
                auto track = m_videoTracks.get(m_currentVideoStreamId);
                ASSERT(track);
                track->markAsActive();
            } else if (type & GST_STREAM_TYPE_TEXT)
                m_currentTextStreamId = streamId;
            else
                GST_WARNING("Unknown stream type with stream-id %s", streamId.utf8().data());
        }
        break;
    }
    default:
        GST_DEBUG_OBJECT(pipeline(), "Unhandled GStreamer message type: %s", GST_MESSAGE_TYPE_NAME(message));
        break;
    }
}

void MediaPlayerPrivateGStreamer::processBufferingStats(GstMessage* message)
{
    GstBufferingMode mode;
    gst_message_parse_buffering_stats(message, &mode, nullptr, nullptr, nullptr);

    int percentage;
    gst_message_parse_buffering(message, &percentage);

    updateBufferingStatus(mode, percentage);
}

void MediaPlayerPrivateGStreamer::updateMaxTimeLoaded(double percentage)
{
    MediaTime mediaDuration = durationMediaTime();
    if (!mediaDuration)
        return;

    m_maxTimeLoaded = MediaTime(percentage * static_cast<double>(toGstUnsigned64Time(mediaDuration)) / 100, GST_SECOND);
    GST_DEBUG_OBJECT(pipeline(), "[Buffering] Updated maxTimeLoaded: %s", toString(m_maxTimeLoaded).utf8().data());
}

void MediaPlayerPrivateGStreamer::updateBufferingStatus(GstBufferingMode mode, double percentage)
{
    GST_DEBUG_OBJECT(pipeline(), "[Buffering] mode: %s, status: %f%%", enumToString(GST_TYPE_BUFFERING_MODE, mode).data(), percentage);

    m_downloadFinished = percentage == 100;
    m_buffering = !m_downloadFinished;

    switch (mode) {
    case GST_BUFFERING_STREAM: {
        updateMaxTimeLoaded(percentage);

        m_bufferingPercentage = percentage;
        if (m_downloadFinished)
            updateStates();

        break;
    }
    case GST_BUFFERING_DOWNLOAD: {
        updateMaxTimeLoaded(percentage);

        // Media is now fully loaded. It will play even if network connection is
        // cut. Buffering is done, remove the fill source from the main loop.
        if (m_downloadFinished)
            m_fillTimer.stop();

        updateStates();
        break;
    }
    default:
        GST_DEBUG_OBJECT(pipeline(), "Unhandled buffering mode: %s", enumToString(GST_TYPE_BUFFERING_MODE, mode).data());
        break;
    }
}

#if ENABLE(VIDEO_TRACK) && USE(GSTREAMER_MPEGTS)
void MediaPlayerPrivateGStreamer::processMpegTsSection(GstMpegtsSection* section)
{
    ASSERT(section);

    if (section->section_type == GST_MPEGTS_SECTION_PMT) {
        const GstMpegtsPMT* pmt = gst_mpegts_section_get_pmt(section);
        m_metadataTracks.clear();
        for (guint i = 0; i < pmt->streams->len; ++i) {
            const GstMpegtsPMTStream* stream = static_cast<const GstMpegtsPMTStream*>(g_ptr_array_index(pmt->streams, i));
            if (stream->stream_type == 0x05 || stream->stream_type >= 0x80) {
                AtomString pid = String::number(stream->pid);
                auto track = InbandMetadataTextTrackPrivateGStreamer::create(
                    InbandTextTrackPrivate::Metadata, InbandTextTrackPrivate::Data, pid);

                // 4.7.10.12.2 Sourcing in-band text tracks
                // If the new text track's kind is metadata, then set the text track in-band metadata track dispatch
                // type as follows, based on the type of the media resource:
                // Let stream type be the value of the "stream_type" field describing the text track's type in the
                // file's program map section, interpreted as an 8-bit unsigned integer. Let length be the value of
                // the "ES_info_length" field for the track in the same part of the program map section, interpreted
                // as an integer as defined by the MPEG-2 specification. Let descriptor bytes be the length bytes
                // following the "ES_info_length" field. The text track in-band metadata track dispatch type must be
                // set to the concatenation of the stream type byte and the zero or more descriptor bytes bytes,
                // expressed in hexadecimal using uppercase ASCII hex digits.
                String inbandMetadataTrackDispatchType;
                appendUnsignedAsHexFixedSize(stream->stream_type, inbandMetadataTrackDispatchType, 2);
                for (guint j = 0; j < stream->descriptors->len; ++j) {
                    const GstMpegtsDescriptor* descriptor = static_cast<const GstMpegtsDescriptor*>(g_ptr_array_index(stream->descriptors, j));
                    for (guint k = 0; k < descriptor->length; ++k)
                        appendByteAsHex(descriptor->data[k], inbandMetadataTrackDispatchType);
                }
                track->setInBandMetadataTrackDispatchType(inbandMetadataTrackDispatchType);

                m_metadataTracks.add(pid, track);
                m_player->addTextTrack(*track);
            }
        }
    } else {
        AtomString pid = String::number(section->pid);
        RefPtr<InbandMetadataTextTrackPrivateGStreamer> track = m_metadataTracks.get(pid);
        if (!track)
            return;

        GRefPtr<GBytes> data = gst_mpegts_section_get_data(section);
        gsize size;
        const void* bytes = g_bytes_get_data(data.get(), &size);

        track->addDataCue(currentMediaTime(), currentMediaTime(), bytes, size);
    }
}
#endif

#if ENABLE(VIDEO_TRACK)
void MediaPlayerPrivateGStreamer::processTableOfContents(GstMessage* message)
{
    if (m_chaptersTrack)
        m_player->removeTextTrack(*m_chaptersTrack);

    m_chaptersTrack = InbandMetadataTextTrackPrivateGStreamer::create(InbandTextTrackPrivate::Chapters, InbandTextTrackPrivate::Generic);
    m_player->addTextTrack(*m_chaptersTrack);

    GRefPtr<GstToc> toc;
    gboolean updated;
    gst_message_parse_toc(message, &toc.outPtr(), &updated);
    ASSERT(toc);

    for (GList* i = gst_toc_get_entries(toc.get()); i; i = i->next)
        processTableOfContentsEntry(static_cast<GstTocEntry*>(i->data));
}

void MediaPlayerPrivateGStreamer::processTableOfContentsEntry(GstTocEntry* entry)
{
    ASSERT(entry);

    auto cue = GenericCueData::create();

    gint64 start = -1, stop = -1;
    gst_toc_entry_get_start_stop_times(entry, &start, &stop);
    if (start != -1)
        cue->setStartTime(MediaTime(start, GST_SECOND));
    if (stop != -1)
        cue->setEndTime(MediaTime(stop, GST_SECOND));

    GstTagList* tags = gst_toc_entry_get_tags(entry);
    if (tags) {
        gchar* title = nullptr;
        gst_tag_list_get_string(tags, GST_TAG_TITLE, &title);
        if (title) {
            cue->setContent(title);
            g_free(title);
        }
    }

    m_chaptersTrack->addGenericCue(cue);

    for (GList* i = gst_toc_entry_get_sub_entries(entry); i; i = i->next)
        processTableOfContentsEntry(static_cast<GstTocEntry*>(i->data));
}

void MediaPlayerPrivateGStreamer::purgeInvalidAudioTracks(Vector<String> validTrackIds)
{
    m_audioTracks.removeIf([validTrackIds](auto& keyAndValue) {
        return !validTrackIds.contains(keyAndValue.key);
    });
}

void MediaPlayerPrivateGStreamer::purgeInvalidVideoTracks(Vector<String> validTrackIds)
{
    m_videoTracks.removeIf([validTrackIds](auto& keyAndValue) {
        return !validTrackIds.contains(keyAndValue.key);
    });
}

void MediaPlayerPrivateGStreamer::purgeInvalidTextTracks(Vector<String> validTrackIds)
{
    m_textTracks.removeIf([validTrackIds](auto& keyAndValue) {
        return !validTrackIds.contains(keyAndValue.key);
    });
}
#endif

void MediaPlayerPrivateGStreamer::uriDecodeBinElementAddedCallback(GstBin* bin, GstElement* element, MediaPlayerPrivateGStreamer* player)
{
    if (g_strcmp0(G_OBJECT_TYPE_NAME(element), "GstDownloadBuffer"))
        return;

    player->m_downloadBuffer = element;
    g_signal_handlers_disconnect_by_func(bin, reinterpret_cast<gpointer>(uriDecodeBinElementAddedCallback), player);
    g_signal_connect_swapped(element, "notify::temp-location", G_CALLBACK(downloadBufferFileCreatedCallback), player);

    GUniqueOutPtr<char> oldDownloadTemplate;
    g_object_get(element, "temp-template", &oldDownloadTemplate.outPtr(), nullptr);

    GUniquePtr<char> newDownloadTemplate(g_build_filename(G_DIR_SEPARATOR_S, "var", "tmp", "WebKit-Media-XXXXXX", nullptr));
    g_object_set(element, "temp-template", newDownloadTemplate.get(), nullptr);
    GST_DEBUG_OBJECT(player->pipeline(), "Reconfigured file download template from '%s' to '%s'", oldDownloadTemplate.get(), newDownloadTemplate.get());

    player->purgeOldDownloadFiles(oldDownloadTemplate.get());
}

void MediaPlayerPrivateGStreamer::downloadBufferFileCreatedCallback(MediaPlayerPrivateGStreamer* player)
{
    ASSERT(player->m_downloadBuffer);

    g_signal_handlers_disconnect_by_func(player->m_downloadBuffer.get(), reinterpret_cast<gpointer>(downloadBufferFileCreatedCallback), player);

    GUniqueOutPtr<char> downloadFile;
    g_object_get(player->m_downloadBuffer.get(), "temp-location", &downloadFile.outPtr(), nullptr);
    player->m_downloadBuffer = nullptr;

    if (UNLIKELY(!FileSystem::deleteFile(downloadFile.get()))) {
        GST_WARNING("Couldn't unlink media temporary file %s after creation", downloadFile.get());
        return;
    }

    GST_DEBUG_OBJECT(player->pipeline(), "Unlinked media temporary file %s after creation", downloadFile.get());
}

void MediaPlayerPrivateGStreamer::purgeOldDownloadFiles(const char* downloadFileTemplate)
{
    if (!downloadFileTemplate)
        return;

    GUniquePtr<char> templatePath(g_path_get_dirname(downloadFileTemplate));
    GUniquePtr<char> templateFile(g_path_get_basename(downloadFileTemplate));
    String templatePattern = String(templateFile.get()).replace("X", "?");

    for (auto& filePath : FileSystem::listDirectory(templatePath.get(), templatePattern)) {
        if (UNLIKELY(!FileSystem::deleteFile(filePath))) {
            GST_WARNING("Couldn't unlink legacy media temporary file: %s", filePath.utf8().data());
            continue;
        }

        GST_TRACE("Unlinked legacy media temporary file: %s", filePath.utf8().data());
    }
}

void MediaPlayerPrivateGStreamer::asyncStateChangeDone()
{
    if (!m_pipeline || m_errorOccured)
        return;

    if (m_seeking) {
        if (m_seekIsPending)
            updateStates();
        else {
            GST_DEBUG_OBJECT(pipeline(), "[Seek] seeked to %s", toString(m_seekTime).utf8().data());
            m_seeking = false;
            m_cachedPosition = MediaTime::invalidTime();
            if (m_timeOfOverlappingSeek != m_seekTime && m_timeOfOverlappingSeek.isValid()) {
                seek(m_timeOfOverlappingSeek);
                m_timeOfOverlappingSeek = MediaTime::invalidTime();
                return;
            }
            m_timeOfOverlappingSeek = MediaTime::invalidTime();

            // The pipeline can still have a pending state. In this case a position query will fail.
            // Right now we can use m_seekTime as a fallback.
            m_canFallBackToLastFinishedSeekPosition = true;
            timeChanged();
        }
    } else
        updateStates();
}

void MediaPlayerPrivateGStreamer::updateStates()
{
    if (!m_pipeline)
        return;

    if (m_errorOccured)
        return;

    MediaPlayer::NetworkState oldNetworkState = m_networkState;
    MediaPlayer::ReadyState oldReadyState = m_readyState;
    GstState pending;
    GstState state;
    bool stateReallyChanged = false;

    GstStateChangeReturn getStateResult = gst_element_get_state(m_pipeline.get(), &state, &pending, 250 * GST_NSECOND);
    if (state != m_currentState) {
        m_oldState = m_currentState;
        m_currentState = state;
        stateReallyChanged = true;
    }

    bool shouldUpdatePlaybackState = false;
    switch (getStateResult) {
    case GST_STATE_CHANGE_SUCCESS: {
        GST_DEBUG_OBJECT(pipeline(), "State: %s, pending: %s", gst_element_state_get_name(m_currentState), gst_element_state_get_name(pending));

        // Do nothing if on EOS and state changed to READY to avoid recreating the player
        // on HTMLMediaElement and properly generate the video 'ended' event.
        if (m_isEndReached && m_currentState == GST_STATE_READY)
            break;

        m_resetPipeline = m_currentState <= GST_STATE_READY;

        bool didBuffering = m_buffering;

        // Update ready and network states.
        switch (m_currentState) {
        case GST_STATE_NULL:
            m_readyState = MediaPlayer::HaveNothing;
            m_networkState = MediaPlayer::Empty;
            break;
        case GST_STATE_READY:
            m_readyState = MediaPlayer::HaveMetadata;
            m_networkState = MediaPlayer::Empty;
            break;
        case GST_STATE_PAUSED:
        case GST_STATE_PLAYING:
            if (m_buffering) {
                if (m_bufferingPercentage == 100) {
                    GST_DEBUG_OBJECT(pipeline(), "[Buffering] Complete.");
                    m_buffering = false;
                    m_readyState = MediaPlayer::HaveEnoughData;
                    m_networkState = m_downloadFinished ? MediaPlayer::Idle : MediaPlayer::Loading;
                } else {
                    m_readyState = MediaPlayer::HaveCurrentData;
                    m_networkState = MediaPlayer::Loading;
                }
            } else if (m_downloadFinished) {
                m_readyState = MediaPlayer::HaveEnoughData;
                m_networkState = MediaPlayer::Loaded;
            } else {
                m_readyState = MediaPlayer::HaveFutureData;
                m_networkState = MediaPlayer::Loading;
            }

            break;
        default:
            ASSERT_NOT_REACHED();
            break;
        }

        // Sync states where needed.
        if (m_currentState == GST_STATE_PAUSED) {
            if (!m_volumeAndMuteInitialized) {
                notifyPlayerOfVolumeChange();
                notifyPlayerOfMute();
                m_volumeAndMuteInitialized = true;
            }

            if (didBuffering && !m_buffering && !m_paused && m_playbackRate) {
                GST_DEBUG_OBJECT(pipeline(), "[Buffering] Restarting playback.");
                changePipelineState(GST_STATE_PLAYING);
            }
        } else if (m_currentState == GST_STATE_PLAYING) {
            m_paused = false;

            if ((m_buffering && !isLiveStream()) || !m_playbackRate) {
                GST_DEBUG_OBJECT(pipeline(), "[Buffering] Pausing stream for buffering.");
                changePipelineState(GST_STATE_PAUSED);
            }
        } else
            m_paused = true;

        GST_DEBUG_OBJECT(pipeline(), "Old state: %s, new state: %s (requested: %s)", gst_element_state_get_name(m_oldState), gst_element_state_get_name(m_currentState), gst_element_state_get_name(m_requestedState));
        if (m_requestedState == GST_STATE_PAUSED && m_currentState == GST_STATE_PAUSED) {
            shouldUpdatePlaybackState = true;
            GST_INFO_OBJECT(pipeline(), "Requested state change to %s was completed", gst_element_state_get_name(m_currentState));
        }

        // Emit play state change notification only when going to PLAYING so that
        // the media element gets a chance to enable its page sleep disabler.
        // Emitting this notification in more cases triggers unwanted code paths
        // and test timeouts.
        if (stateReallyChanged && (m_oldState != m_currentState) && (m_oldState == GST_STATE_PAUSED && m_currentState == GST_STATE_PLAYING)) {
            GST_INFO_OBJECT(pipeline(), "Playback state changed from %s to %s. Notifying the media player client", gst_element_state_get_name(m_oldState), gst_element_state_get_name(m_currentState));
            shouldUpdatePlaybackState = true;
        }

        break;
    }
    case GST_STATE_CHANGE_ASYNC:
        GST_DEBUG_OBJECT(pipeline(), "Async: State: %s, pending: %s", gst_element_state_get_name(m_currentState), gst_element_state_get_name(pending));
        // Change in progress.
        break;
    case GST_STATE_CHANGE_FAILURE:
        GST_DEBUG_OBJECT(pipeline(), "Failure: State: %s, pending: %s", gst_element_state_get_name(m_currentState), gst_element_state_get_name(pending));
        // Change failed
        return;
    case GST_STATE_CHANGE_NO_PREROLL:
        GST_DEBUG_OBJECT(pipeline(), "No preroll: State: %s, pending: %s", gst_element_state_get_name(m_currentState), gst_element_state_get_name(pending));

        // Live pipelines go in PAUSED without prerolling.
        m_isStreaming = true;
        updateDownloadBufferingFlag();

        if (m_currentState == GST_STATE_READY)
            m_readyState = MediaPlayer::HaveNothing;
        else if (m_currentState == GST_STATE_PAUSED) {
            m_readyState = MediaPlayer::HaveEnoughData;
            m_paused = true;
        } else if (m_currentState == GST_STATE_PLAYING)
            m_paused = false;

        if (!m_paused && m_playbackRate)
            changePipelineState(GST_STATE_PLAYING);

        m_networkState = MediaPlayer::Loading;
        break;
    default:
        GST_DEBUG_OBJECT(pipeline(), "Else : %d", getStateResult);
        break;
    }

    m_requestedState = GST_STATE_VOID_PENDING;

    if (shouldUpdatePlaybackState)
        m_player->playbackStateChanged();

    if (m_networkState != oldNetworkState) {
        GST_DEBUG_OBJECT(pipeline(), "Network State Changed from %s to %s", convertEnumerationToString(oldNetworkState).utf8().data(), convertEnumerationToString(m_networkState).utf8().data());
        m_player->networkStateChanged();
    }
    if (m_readyState != oldReadyState) {
        GST_DEBUG_OBJECT(pipeline(), "Ready State Changed from %s to %s", convertEnumerationToString(oldReadyState).utf8().data(), convertEnumerationToString(m_readyState).utf8().data());
        m_player->readyStateChanged();
    }

    if (getStateResult == GST_STATE_CHANGE_SUCCESS && m_currentState >= GST_STATE_PAUSED) {
        updatePlaybackRate();
        if (m_seekIsPending) {
            GST_DEBUG_OBJECT(pipeline(), "[Seek] committing pending seek to %s", toString(m_seekTime).utf8().data());
            m_seekIsPending = false;
            m_seeking = doSeek(m_seekTime, m_player->rate(), static_cast<GstSeekFlags>(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_ACCURATE));
            if (!m_seeking) {
                m_cachedPosition = MediaTime::invalidTime();
                GST_DEBUG_OBJECT(pipeline(), "[Seek] seeking to %s failed", toString(m_seekTime).utf8().data());
            }
        }
    }
}

void MediaPlayerPrivateGStreamer::mediaLocationChanged(GstMessage* message)
{
    if (m_mediaLocations)
        gst_structure_free(m_mediaLocations);

    const GstStructure* structure = gst_message_get_structure(message);
    if (structure) {
        // This structure can contain:
        // - both a new-location string and embedded locations structure
        // - or only a new-location string.
        m_mediaLocations = gst_structure_copy(structure);
        const GValue* locations = gst_structure_get_value(m_mediaLocations, "locations");

        if (locations)
            m_mediaLocationCurrentIndex = static_cast<int>(gst_value_list_get_size(locations)) -1;

        loadNextLocation();
    }
}

bool MediaPlayerPrivateGStreamer::loadNextLocation()
{
    if (!m_mediaLocations)
        return false;

    const GValue* locations = gst_structure_get_value(m_mediaLocations, "locations");
    const gchar* newLocation = nullptr;

    if (!locations) {
        // Fallback on new-location string.
        newLocation = gst_structure_get_string(m_mediaLocations, "new-location");
        if (!newLocation)
            return false;
    }

    if (!newLocation) {
        if (m_mediaLocationCurrentIndex < 0) {
            m_mediaLocations = nullptr;
            return false;
        }

        const GValue* location = gst_value_list_get_value(locations, m_mediaLocationCurrentIndex);
        const GstStructure* structure = gst_value_get_structure(location);

        if (!structure) {
            m_mediaLocationCurrentIndex--;
            return false;
        }

        newLocation = gst_structure_get_string(structure, "new-location");
    }

    if (newLocation) {
        // Found a candidate. new-location is not always an absolute url
        // though. We need to take the base of the current url and
        // append the value of new-location to it.
        URL baseUrl = gst_uri_is_valid(newLocation) ? URL() : m_url;
        URL newUrl = URL(baseUrl, newLocation);

        GUniqueOutPtr<gchar> playbinUrlStr;
        g_object_get(m_pipeline.get(), "current-uri", &playbinUrlStr.outPtr(), nullptr);
        URL playbinUrl(URL(), playbinUrlStr.get());

        if (playbinUrl == newUrl) {
            GST_DEBUG_OBJECT(pipeline(), "Playbin already handled redirection.");

            m_url = playbinUrl;

            return true;
        }

        changePipelineState(GST_STATE_READY);
        auto securityOrigin = SecurityOrigin::create(m_url);
        if (securityOrigin->canRequest(newUrl)) {
            GST_INFO_OBJECT(pipeline(), "New media url: %s", newUrl.string().utf8().data());

            // Reset player states.
            m_networkState = MediaPlayer::Loading;
            m_player->networkStateChanged();
            m_readyState = MediaPlayer::HaveNothing;
            m_player->readyStateChanged();

            // Reset pipeline state.
            m_resetPipeline = true;

            GstState state;
            gst_element_get_state(m_pipeline.get(), &state, nullptr, 0);
            if (state <= GST_STATE_READY) {
                // Set the new uri and start playing.
                setPlaybinURL(newUrl);
                changePipelineState(GST_STATE_PLAYING);
                return true;
            }
        } else
            GST_INFO_OBJECT(pipeline(), "Not allowed to load new media location: %s", newUrl.string().utf8().data());
    }
    m_mediaLocationCurrentIndex--;
    return false;
}

void MediaPlayerPrivateGStreamer::didEnd()
{
    GST_INFO_OBJECT(pipeline(), "Playback ended");

    // Synchronize position and duration values to not confuse the
    // HTMLMediaElement. In some cases like reverse playback the
    // position is not always reported as 0 for instance.
    m_cachedPosition = MediaTime::invalidTime();
    MediaTime now = currentMediaTime();
    if (now > MediaTime::zeroTime() && !m_seeking) {
        m_cachedDuration = now;
        m_player->durationChanged();
    }

    m_isEndReached = true;

    if (!m_player->client().mediaPlayerIsLooping()) {
        m_paused = true;
        changePipelineState(GST_STATE_READY);
        m_downloadFinished = false;
    }
    timeChanged();
}

void MediaPlayerPrivateGStreamer::getSupportedTypes(HashSet<String, ASCIICaseInsensitiveHash>& types)
{
    auto& gstRegistryScanner = GStreamerRegistryScanner::singleton();
    types = gstRegistryScanner.mimeTypeSet();
}

MediaPlayer::SupportsType MediaPlayerPrivateGStreamer::supportsType(const MediaEngineSupportParameters& parameters)
{
    MediaPlayer::SupportsType result = MediaPlayer::IsNotSupported;
#if ENABLE(MEDIA_SOURCE)
    // MediaPlayerPrivateGStreamerMSE is in charge of mediasource playback, not us.
    if (parameters.isMediaSource)
        return result;
#endif

#if !ENABLE(MEDIA_STREAM)
    if (parameters.isMediaStream)
        return result;
#endif

    if (parameters.type.isEmpty())
        return result;

    GST_DEBUG("Checking mime-type \"%s\"", parameters.type.raw().utf8().data());
    auto containerType = parameters.type.containerType();
    auto& gstRegistryScanner = GStreamerRegistryScanner::singleton();
    if (gstRegistryScanner.isContainerTypeSupported(containerType)) {
        // Spec says we should not return "probably" if the codecs string is empty.
        Vector<String> codecs = parameters.type.codecs();
        result = codecs.isEmpty() ? MediaPlayer::MayBeSupported : (gstRegistryScanner.areAllCodecsSupported(codecs) ? MediaPlayer::IsSupported : MediaPlayer::IsNotSupported);
    }

    auto finalResult = extendedSupportsType(parameters, result);
    GST_DEBUG("Supported: %s", convertEnumerationToString(finalResult).utf8().data());
    return finalResult;
}

void MediaPlayerPrivateGStreamer::updateDownloadBufferingFlag()
{
    if (!m_pipeline)
        return;

    unsigned flags;
    g_object_get(m_pipeline.get(), "flags", &flags, nullptr);

    unsigned flagDownload = getGstPlayFlag("download");

    // We don't want to stop downloading if we already started it.
    if (flags & flagDownload && m_readyState > MediaPlayer::HaveNothing && !m_resetPipeline) {
        GST_DEBUG_OBJECT(pipeline(), "Download already started, not starting again");
        return;
    }

    bool shouldDownload = !isLiveStream() && m_preload == MediaPlayer::Auto;
    if (shouldDownload) {
        GST_INFO_OBJECT(pipeline(), "Enabling on-disk buffering");
        g_object_set(m_pipeline.get(), "flags", flags | flagDownload, nullptr);
        m_fillTimer.startRepeating(200_ms);
    } else {
        GST_INFO_OBJECT(pipeline(), "Disabling on-disk buffering");
        g_object_set(m_pipeline.get(), "flags", flags & ~flagDownload, nullptr);
        m_fillTimer.stop();
    }
}

void MediaPlayerPrivateGStreamer::createGSTPlayBin(const URL& url, const String& pipelineName)
{
    const gchar* playbinName = "playbin";

    // MSE doesn't support playbin3. Mediastream requires playbin3. Regular
    // playback can use playbin3 on-demand with the WEBKIT_GST_USE_PLAYBIN3
    // environment variable.
    if ((!isMediaSource() && g_getenv("WEBKIT_GST_USE_PLAYBIN3")) || url.protocolIs("mediastream"))
        playbinName = "playbin3";

    if (m_pipeline) {
        if (!g_strcmp0(GST_OBJECT_NAME(gst_element_get_factory(m_pipeline.get())), playbinName)) {
            GST_INFO_OBJECT(pipeline(), "Already using %s", playbinName);
            return;
        }

        GST_INFO_OBJECT(pipeline(), "Tearing down as we need to use %s now.", playbinName);
        changePipelineState(GST_STATE_NULL);
        m_pipeline = nullptr;
    }

    ASSERT(!m_pipeline);

    m_isLegacyPlaybin = !g_strcmp0(playbinName, "playbin");

    // gst_element_factory_make() returns a floating reference so
    // we should not adopt.
    static Atomic<uint32_t> pipelineId;
    setPipeline(gst_element_factory_make(playbinName,
        (pipelineName.isEmpty() ? makeString("media-player-", pipelineId.exchangeAdd(1)) : pipelineName).utf8().data()));
    setStreamVolumeElement(GST_STREAM_VOLUME(m_pipeline.get()));

    GST_INFO_OBJECT(pipeline(), "Using legacy playbin element: %s", boolForPrinting(m_isLegacyPlaybin));

    // Let also other listeners subscribe to (application) messages in this bus.
    GRefPtr<GstBus> bus = adoptGRef(gst_pipeline_get_bus(GST_PIPELINE(m_pipeline.get())));
    gst_bus_add_signal_watch_full(bus.get(), RunLoopSourcePriority::RunLoopDispatcher);
    g_signal_connect(bus.get(), "message", G_CALLBACK(busMessageCallback), this);

    g_object_set(m_pipeline.get(), "mute", m_player->muted(), nullptr);

    g_signal_connect(GST_BIN_CAST(m_pipeline.get()), "deep-element-added", G_CALLBACK(+[](GstBin*, GstBin* subBin, GstElement* element, MediaPlayerPrivateGStreamer* player) {
        GUniquePtr<char> binName(gst_element_get_name(GST_ELEMENT_CAST(subBin)));
        if (!g_str_has_prefix(binName.get(), "decodebin"))
            return;

        GUniquePtr<char> elementName(gst_element_get_name(element));
        if (g_str_has_prefix(elementName.get(), "v4l2"))
            player->m_videoDecoderPlatform = GstVideoDecoderPlatform::Video4Linux;
        else if (g_str_has_prefix(elementName.get(), "imxvpudec"))
            player->m_videoDecoderPlatform = GstVideoDecoderPlatform::ImxVPU;

#if USE(TEXTURE_MAPPER_GL)
        player->updateTextureMapperFlags();
#endif
    }), this);

    g_signal_connect_swapped(m_pipeline.get(), "source-setup", G_CALLBACK(sourceSetupCallback), this);
    if (m_isLegacyPlaybin) {
        g_signal_connect_swapped(m_pipeline.get(), "video-changed", G_CALLBACK(videoChangedCallback), this);
        g_signal_connect_swapped(m_pipeline.get(), "audio-changed", G_CALLBACK(audioChangedCallback), this);
    }

#if ENABLE(VIDEO_TRACK)
    if (m_isLegacyPlaybin)
        g_signal_connect_swapped(m_pipeline.get(), "text-changed", G_CALLBACK(textChangedCallback), this);

    GstElement* textCombiner = webkitTextCombinerNew();
    ASSERT(textCombiner);
    g_object_set(m_pipeline.get(), "text-stream-combiner", textCombiner, nullptr);

    m_textAppSink = webkitTextSinkNew();
    ASSERT(m_textAppSink);

    m_textAppSinkPad = adoptGRef(gst_element_get_static_pad(m_textAppSink.get(), "sink"));
    ASSERT(m_textAppSinkPad);

    GRefPtr<GstCaps> textCaps;
    if (webkitGstCheckVersion(1, 14, 0))
        textCaps = adoptGRef(gst_caps_new_empty_simple("application/x-subtitle-vtt"));
    else
        textCaps = adoptGRef(gst_caps_new_empty_simple("text/vtt"));
    g_object_set(m_textAppSink.get(), "emit-signals", TRUE, "enable-last-sample", FALSE, "caps", textCaps.get(), nullptr);
    g_signal_connect_swapped(m_textAppSink.get(), "new-sample", G_CALLBACK(newTextSampleCallback), this);

    g_object_set(m_pipeline.get(), "text-sink", m_textAppSink.get(), nullptr);
#endif

    g_object_set(m_pipeline.get(), "video-sink", createVideoSink(), "audio-sink", createAudioSink(), nullptr);

    configurePlaySink();

    if (m_preservesPitch) {
        GstElement* scale = gst_element_factory_make("scaletempo", nullptr);

        if (!scale)
            GST_WARNING("Failed to create scaletempo");
        else
            g_object_set(m_pipeline.get(), "audio-filter", scale, nullptr);
    }

    if (!m_renderingCanBeAccelerated) {
        // If not using accelerated compositing, let GStreamer handle
        // the image-orientation tag.
        GstElement* videoFlip = gst_element_factory_make("videoflip", nullptr);
        if (videoFlip) {
            g_object_set(videoFlip, "method", 8, nullptr);
            g_object_set(m_pipeline.get(), "video-filter", videoFlip, nullptr);
        } else
            GST_WARNING("The videoflip element is missing, video rotation support is now disabled. Please check your gst-plugins-good installation.");
    }

    GRefPtr<GstPad> videoSinkPad = adoptGRef(gst_element_get_static_pad(m_videoSink.get(), "sink"));
    if (videoSinkPad)
        g_signal_connect_swapped(videoSinkPad.get(), "notify::caps", G_CALLBACK(videoSinkCapsChangedCallback), this);
}

bool MediaPlayerPrivateGStreamer::didPassCORSAccessCheck() const
{
    if (WEBKIT_IS_WEB_SRC(m_source.get()))
        return webKitSrcPassedCORSAccessCheck(WEBKIT_WEB_SRC_CAST(m_source.get()));
    return false;
}

bool MediaPlayerPrivateGStreamer::canSaveMediaData() const
{
    if (isLiveStream())
        return false;

    if (m_url.isLocalFile())
        return true;

    if (m_url.protocolIsInHTTPFamily())
        return true;

    return false;
}

void MediaPlayerPrivateGStreamer::readyTimerFired()
{
    GST_DEBUG_OBJECT(pipeline(), "In READY for too long. Releasing pipeline resources.");
    changePipelineState(GST_STATE_NULL);
}

void MediaPlayerPrivateGStreamer::acceleratedRenderingStateChanged()
{
    m_renderingCanBeAccelerated = m_player && m_player->client().mediaPlayerAcceleratedCompositingEnabled();
}

#if USE(TEXTURE_MAPPER_GL)
PlatformLayer* MediaPlayerPrivateGStreamer::platformLayer() const
{
#if USE(NICOSIA)
    return m_nicosiaLayer.ptr();
#else
    return const_cast<MediaPlayerPrivateGStreamer*>(this);
#endif
}

#if USE(NICOSIA)
void MediaPlayerPrivateGStreamer::swapBuffersIfNeeded()
{
#if USE(GSTREAMER_HOLEPUNCH)
    pushNextHolePunchBuffer();
#endif
}
#else
RefPtr<TextureMapperPlatformLayerProxy> MediaPlayerPrivateGStreamer::proxy() const
{
    return m_platformLayerProxy.copyRef();
}

void MediaPlayerPrivateGStreamer::swapBuffersIfNeeded()
{
#if USE(GSTREAMER_HOLEPUNCH)
    pushNextHolePunchBuffer();
#endif
}
#endif

void MediaPlayerPrivateGStreamer::pushTextureToCompositor()
{
    auto sampleLocker = holdLock(m_sampleMutex);
    if (!GST_IS_SAMPLE(m_sample.get()))
        return;

    auto proxyOperation =
        [this](TextureMapperPlatformLayerProxy& proxy)
        {
            LockHolder holder(proxy.lock());

            if (!proxy.isActive())
                return;

            std::unique_ptr<GstVideoFrameHolder> frameHolder = makeUnique<GstVideoFrameHolder>(m_sample.get(), m_videoDecoderPlatform, m_textureMapperFlags, !m_usingFallbackVideoSink);

            std::unique_ptr<TextureMapperPlatformLayerBuffer> layerBuffer;
            if (frameHolder->hasMappedTextures()) {
                layerBuffer = frameHolder->platformLayerBuffer();
                if (!layerBuffer)
                    return;
                layerBuffer->setUnmanagedBufferDataHolder(WTFMove(frameHolder));
            } else {
                layerBuffer = proxy.getAvailableBuffer(frameHolder->size(), GL_DONT_CARE);
                if (UNLIKELY(!layerBuffer)) {
                    auto texture = BitmapTextureGL::create(TextureMapperContextAttributes::get());
                    texture->reset(frameHolder->size(), frameHolder->hasAlphaChannel() ? BitmapTexture::SupportsAlpha : BitmapTexture::NoFlag);
                    layerBuffer = makeUnique<TextureMapperPlatformLayerBuffer>(WTFMove(texture));
                }
                frameHolder->updateTexture(layerBuffer->textureGL());
                layerBuffer->setExtraFlags(m_textureMapperFlags | (frameHolder->hasAlphaChannel() ? TextureMapperGL::ShouldBlend : 0));
            }
            proxy.pushNextBuffer(WTFMove(layerBuffer));
        };

#if USE(NICOSIA)
    proxyOperation(downcast<Nicosia::ContentLayerTextureMapperImpl>(m_nicosiaLayer->impl()).proxy());
#else
    proxyOperation(*m_platformLayerProxy);
#endif
}
#endif // USE(TEXTURE_MAPPER_GL)

void MediaPlayerPrivateGStreamer::repaint()
{
    ASSERT(m_sample);
    ASSERT(isMainThread());

    m_player->repaint();

    LockHolder lock(m_drawMutex);
    m_drawCondition.notifyOne();
}

void MediaPlayerPrivateGStreamer::triggerRepaint(GstSample* sample)
{
    bool triggerResize;
    {
        auto sampleLocker = holdLock(m_sampleMutex);
        triggerResize = !m_sample;
        m_sample = sample;
    }

    if (triggerResize) {
        GST_DEBUG_OBJECT(pipeline(), "First sample reached the sink, triggering video dimensions update");
        m_notifier->notify(MainThreadNotification::SizeChanged, [this] {
            m_player->sizeChanged();
        });
    }

    if (!m_renderingCanBeAccelerated) {
        LockHolder locker(m_drawMutex);
        if (m_destroying)
            return;
        m_drawTimer.startOneShot(0_s);
        m_drawCondition.wait(m_drawMutex);
        return;
    }

#if USE(TEXTURE_MAPPER_GL)
    if (m_usingFallbackVideoSink) {
        LockHolder lock(m_drawMutex);
        auto proxyOperation =
            [this](TextureMapperPlatformLayerProxy& proxy)
            {
                return proxy.scheduleUpdateOnCompositorThread([this] { this->pushTextureToCompositor(); });
            };
#if USE(NICOSIA)
        if (!proxyOperation(downcast<Nicosia::ContentLayerTextureMapperImpl>(m_nicosiaLayer->impl()).proxy()))
            return;
#else
        if (!proxyOperation(*m_platformLayerProxy))
            return;
#endif
        m_drawTimer.startOneShot(0_s);
        m_drawCondition.wait(m_drawMutex);
    } else
        pushTextureToCompositor();
#endif // USE(TEXTURE_MAPPER_GL)
}

void MediaPlayerPrivateGStreamer::repaintCallback(MediaPlayerPrivateGStreamer* player, GstSample* sample)
{
    player->triggerRepaint(sample);
}

void MediaPlayerPrivateGStreamer::cancelRepaint(bool destroying)
{
    // The goal of this function is to release the GStreamer thread from m_drawCondition in triggerRepaint() in non-AC case,
    // to avoid a deadlock if the player gets paused while waiting for drawing (see https://bugs.webkit.org/show_bug.cgi?id=170003):
    // the main thread is waiting for the GStreamer thread to pause, but the GStreamer thread is locked waiting for the
    // main thread to draw. This deadlock doesn't happen when using AC because the sample is processed (not painted) in the compositor
    // thread, so the main thread can request the pause and wait if the GStreamer thread is waiting for the compositor thread.
    //
    // This function is also used when destroying the player (destroying parameter is true), to release the gstreamer thread from
    // m_drawCondition and to ensure that new triggerRepaint calls won't wait on m_drawCondition.
    if (!m_renderingCanBeAccelerated) {
        LockHolder locker(m_drawMutex);
        m_drawTimer.stop();
        m_destroying = destroying;
        m_drawCondition.notifyOne();
    }
}

void MediaPlayerPrivateGStreamer::repaintCancelledCallback(MediaPlayerPrivateGStreamer* player)
{
    player->cancelRepaint();
}

#if USE(GSTREAMER_GL)
GstFlowReturn MediaPlayerPrivateGStreamer::newSampleCallback(GstElement* sink, MediaPlayerPrivateGStreamer* player)
{
    GRefPtr<GstSample> sample = adoptGRef(gst_app_sink_pull_sample(GST_APP_SINK(sink)));
    player->triggerRepaint(sample.get());
    return GST_FLOW_OK;
}

GstFlowReturn MediaPlayerPrivateGStreamer::newPrerollCallback(GstElement* sink, MediaPlayerPrivateGStreamer* player)
{
    GRefPtr<GstSample> sample = adoptGRef(gst_app_sink_pull_preroll(GST_APP_SINK(sink)));
    player->triggerRepaint(sample.get());
    return GST_FLOW_OK;
}

void MediaPlayerPrivateGStreamer::flushCurrentBuffer()
{
    auto sampleLocker = holdLock(m_sampleMutex);

    if (m_sample) {
        // Replace by a new sample having only the caps, so this dummy sample is still useful to get the dimensions.
        // This prevents resizing problems when the video changes its quality and a DRAIN is performed.
        const GstStructure* info = gst_sample_get_info(m_sample.get());
        m_sample = adoptGRef(gst_sample_new(nullptr, gst_sample_get_caps(m_sample.get()),
            gst_sample_get_segment(m_sample.get()), info ? gst_structure_copy(info) : nullptr));
    }

    bool shouldWait = m_videoDecoderPlatform == GstVideoDecoderPlatform::Video4Linux;
    auto proxyOperation = [shouldWait, pipeline = pipeline()](TextureMapperPlatformLayerProxy& proxy) {
        GST_DEBUG_OBJECT(pipeline, "Flushing video sample %s", shouldWait ? "synchronously" : "");
        LockHolder locker(!shouldWait ? &proxy.lock() : nullptr);

        if (proxy.isActive())
            proxy.dropCurrentBufferWhilePreservingTexture(shouldWait);
    };

#if USE(NICOSIA)
    proxyOperation(downcast<Nicosia::ContentLayerTextureMapperImpl>(m_nicosiaLayer->impl()).proxy());
#else
    proxyOperation(*m_platformLayerProxy);
#endif
}
#endif

void MediaPlayerPrivateGStreamer::setSize(const IntSize& size)
{
    m_size = size;
}

void MediaPlayerPrivateGStreamer::paint(GraphicsContext& context, const FloatRect& rect)
{
    if (context.paintingDisabled())
        return;

    if (!m_player->visible())
        return;

    auto sampleLocker = holdLock(m_sampleMutex);
    if (!GST_IS_SAMPLE(m_sample.get()))
        return;

#if USE(GSTREAMER_GL)
    // Ensure the input is RGBA. We handle YUV video natively, so we need to do
    // this conversion on-demand here.
    GstBuffer* buffer = gst_sample_get_buffer(m_sample.get());
    if (UNLIKELY(!GST_IS_BUFFER(buffer)))
        return;

    GstCaps* caps = gst_sample_get_caps(m_sample.get());

    GstVideoInfo videoInfo;
    gst_video_info_init(&videoInfo);
    if (!gst_video_info_from_caps(&videoInfo, caps))
        return;

    if (!GST_VIDEO_INFO_IS_RGB(&videoInfo)) {
        if (!m_colorConvert) {
            GstMemory* mem = gst_buffer_peek_memory(buffer, 0);
            GstGLContext* context = ((GstGLBaseMemory*)mem)->context;
            m_colorConvert = adoptGRef(gst_gl_color_convert_new(context));
        }

        if (!m_colorConvertInputCaps || !gst_caps_is_equal(m_colorConvertInputCaps.get(), caps)) {
            m_colorConvertInputCaps = caps;
            m_colorConvertOutputCaps = adoptGRef(gst_caps_copy(caps));
#if G_BYTE_ORDER == G_LITTLE_ENDIAN
            const gchar* formatString = GST_VIDEO_INFO_HAS_ALPHA(&videoInfo) ? "RGBA" : "BGRx";
#else
            const gchar* formatString = GST_VIDEO_INFO_HAS_ALPHA(&videoInfo) ? "RGBA" : "RGBx";
#endif
            gst_caps_set_simple(m_colorConvertOutputCaps.get(), "format", G_TYPE_STRING, formatString, nullptr);
            if (!gst_gl_color_convert_set_caps(m_colorConvert.get(), caps, m_colorConvertOutputCaps.get()))
                return;
        }

        GRefPtr<GstBuffer> rgbBuffer = adoptGRef(gst_gl_color_convert_perform(m_colorConvert.get(), buffer));
        if (UNLIKELY(!GST_IS_BUFFER(rgbBuffer.get())))
            return;

        const GstStructure* info = gst_sample_get_info(m_sample.get());
        m_sample = adoptGRef(gst_sample_new(rgbBuffer.get(), m_colorConvertOutputCaps.get(),
            gst_sample_get_segment(m_sample.get()), info ? gst_structure_copy(info) : nullptr));
    }
#endif

    auto gstImage = ImageGStreamer::createImage(m_sample.get());
    if (!gstImage)
        return;

    context.drawImage(gstImage->image(), rect, gstImage->rect(), { CompositeCopy, m_renderingCanBeAccelerated ? m_videoSourceOrientation : ImageOrientation() });
}

#if USE(GSTREAMER_GL)
bool MediaPlayerPrivateGStreamer::copyVideoTextureToPlatformTexture(GraphicsContext3D* context, Platform3DObject outputTexture, GC3Denum outputTarget, GC3Dint level, GC3Denum internalFormat, GC3Denum format, GC3Denum type, bool premultiplyAlpha, bool flipY)
{
    UNUSED_PARAM(context);

    if (m_usingFallbackVideoSink)
        return false;

    if (premultiplyAlpha)
        return false;

    auto sampleLocker = holdLock(m_sampleMutex);

    if (!GST_IS_SAMPLE(m_sample.get()))
        return false;

    std::unique_ptr<GstVideoFrameHolder> frameHolder = makeUnique<GstVideoFrameHolder>(m_sample.get(), m_videoDecoderPlatform, m_textureMapperFlags, true);

    std::unique_ptr<TextureMapperPlatformLayerBuffer> layerBuffer = frameHolder->platformLayerBuffer();
    if (!layerBuffer)
        return false;

    auto size = frameHolder->size();
    if (m_videoSourceOrientation.usesWidthAsHeight())
        size = size.transposedSize();

    if (!m_videoTextureCopier)
        m_videoTextureCopier = makeUnique<VideoTextureCopierGStreamer>(TEXTURE_COPIER_COLOR_CONVERT_FLAG);

    frameHolder->waitForCPUSync();

    return m_videoTextureCopier->copyVideoTextureToPlatformTexture(*layerBuffer.get(), size, outputTexture, outputTarget, level, internalFormat, format, type, flipY, m_videoSourceOrientation);
}

NativeImagePtr MediaPlayerPrivateGStreamer::nativeImageForCurrentTime()
{
#if USE(CAIRO) && ENABLE(ACCELERATED_2D_CANVAS)
    if (m_usingFallbackVideoSink)
        return nullptr;

    auto sampleLocker = holdLock(m_sampleMutex);

    if (!GST_IS_SAMPLE(m_sample.get()))
        return nullptr;

    std::unique_ptr<GstVideoFrameHolder> frameHolder = makeUnique<GstVideoFrameHolder>(m_sample.get(), m_videoDecoderPlatform, m_textureMapperFlags, true);

    std::unique_ptr<TextureMapperPlatformLayerBuffer> layerBuffer = frameHolder->platformLayerBuffer();
    if (!layerBuffer)
        return nullptr;

    auto size = frameHolder->size();
    if (m_videoSourceOrientation.usesWidthAsHeight())
        size = size.transposedSize();

    GLContext* context = PlatformDisplay::sharedDisplayForCompositing().sharingGLContext();
    context->makeContextCurrent();

    if (!m_videoTextureCopier)
        m_videoTextureCopier = makeUnique<VideoTextureCopierGStreamer>(TEXTURE_COPIER_COLOR_CONVERT_FLAG);

    frameHolder->waitForCPUSync();

    if (!m_videoTextureCopier->copyVideoTextureToPlatformTexture(*layerBuffer.get(), size, 0, GL_TEXTURE_2D, 0, GL_RGBA, GL_RGBA, GL_UNSIGNED_BYTE, false, m_videoSourceOrientation))
        return nullptr;

    return adoptRef(cairo_gl_surface_create_for_texture(context->cairoDevice(), CAIRO_CONTENT_COLOR_ALPHA, m_videoTextureCopier->resultTexture(), size.width(), size.height()));
#else
    return nullptr;
#endif
}
#endif // USE(GSTREAMER_GL)

void MediaPlayerPrivateGStreamer::setVideoSourceOrientation(ImageOrientation orientation)
{
    if (m_videoSourceOrientation == orientation)
        return;

    m_videoSourceOrientation = orientation;
#if USE(TEXTURE_MAPPER_GL)
    updateTextureMapperFlags();
#endif
}

#if USE(TEXTURE_MAPPER_GL)
void MediaPlayerPrivateGStreamer::updateTextureMapperFlags()
{
    switch (m_videoSourceOrientation) {
    case ImageOrientation::OriginTopLeft:
        m_textureMapperFlags = 0;
        break;
    case ImageOrientation::OriginRightTop:
        m_textureMapperFlags = TextureMapperGL::ShouldRotateTexture90;
        break;
    case ImageOrientation::OriginBottomRight:
        m_textureMapperFlags = TextureMapperGL::ShouldRotateTexture180;
        break;
    case ImageOrientation::OriginLeftBottom:
        m_textureMapperFlags = TextureMapperGL::ShouldRotateTexture270;
        break;
    default:
        // FIXME: Handle OriginTopRight, OriginBottomLeft, OriginLeftTop and OriginRightBottom?
        m_textureMapperFlags = 0;
        break;
    }
}
#endif

bool MediaPlayerPrivateGStreamer::supportsFullscreen() const
{
    return true;
}

MediaPlayer::MovieLoadType MediaPlayerPrivateGStreamer::movieLoadType() const
{
    if (m_readyState == MediaPlayer::HaveNothing)
        return MediaPlayer::Unknown;

    if (isLiveStream())
        return MediaPlayer::LiveStream;

    return MediaPlayer::Download;
}

#if USE(GSTREAMER_GL)
GstElement* MediaPlayerPrivateGStreamer::createGLAppSink()
{
    GstElement* appsink = gst_element_factory_make("appsink", "webkit-gl-video-sink");
    if (!appsink)
        return nullptr;

    g_object_set(appsink, "enable-last-sample", FALSE, "emit-signals", TRUE, "max-buffers", 1, nullptr);
    g_signal_connect(appsink, "new-sample", G_CALLBACK(newSampleCallback), this);
    g_signal_connect(appsink, "new-preroll", G_CALLBACK(newPrerollCallback), this);

    GRefPtr<GstPad> pad = adoptGRef(gst_element_get_static_pad(appsink, "sink"));
    gst_pad_add_probe(pad.get(), static_cast<GstPadProbeType>(GST_PAD_PROBE_TYPE_PUSH | GST_PAD_PROBE_TYPE_QUERY_DOWNSTREAM | GST_PAD_PROBE_TYPE_EVENT_FLUSH), [] (GstPad*, GstPadProbeInfo* info,  gpointer userData) -> GstPadProbeReturn {
        // In some platforms (e.g. OpenMAX on the Raspberry Pi) when a resolution change occurs the
        // pipeline has to be drained before a frame with the new resolution can be decoded.
        // In this context, it's important that we don't hold references to any previous frame
        // (e.g. m_sample) so that decoding can continue.
        // We are also not supposed to keep the original frame after a flush.
        if (info->type & GST_PAD_PROBE_TYPE_QUERY_DOWNSTREAM) {
            if (GST_QUERY_TYPE(GST_PAD_PROBE_INFO_QUERY(info)) != GST_QUERY_DRAIN)
                return GST_PAD_PROBE_OK;
            GST_DEBUG("Acting upon DRAIN query");
        }
        if (info->type & GST_PAD_PROBE_TYPE_EVENT_FLUSH) {
            if (GST_EVENT_TYPE(GST_PAD_PROBE_INFO_EVENT(info)) != GST_EVENT_FLUSH_START)
                return GST_PAD_PROBE_OK;
            GST_DEBUG("Acting upon flush-start event");
        }

        auto* player = static_cast<MediaPlayerPrivateGStreamer*>(userData);
        player->flushCurrentBuffer();
        return GST_PAD_PROBE_OK;
    }, this, nullptr);

    return appsink;
}

GstElement* MediaPlayerPrivateGStreamer::createVideoSinkGL()
{
    gboolean result = TRUE;
    GstElement* videoSink = gst_bin_new(nullptr);
    GstElement* upload = gst_element_factory_make("glupload", nullptr);
    GstElement* colorconvert = gst_element_factory_make("glcolorconvert", nullptr);
    GstElement* appsink = createGLAppSink();

    // glsinkbin is not used because it includes glcolorconvert which only process RGBA,
    // but we can display YUV formats too.

    if (!appsink || !upload || !colorconvert) {
        GST_WARNING("Failed to create GstGL elements");
        gst_object_unref(videoSink);

        if (upload)
            gst_object_unref(upload);
        if (colorconvert)
            gst_object_unref(colorconvert);
        if (appsink)
            gst_object_unref(appsink);

        g_warning("WebKit wasn't able to find the GStreamer opengl plugin. Hardware-accelerated zero-copy video rendering can't be enabled without this plugin.");
        return nullptr;
    }

    gst_bin_add_many(GST_BIN(videoSink), upload, colorconvert, appsink, nullptr);

    // Workaround until we can depend on GStreamer 1.16.2.
    // https://gitlab.freedesktop.org/gstreamer/gst-plugins-base/commit/8d32de090554cf29fe359f83aa46000ba658a693
    // Forcing a color conversion to RGBA here allows glupload to internally use
    // an uploader that adds a VideoMeta, through the TextureUploadMeta caps
    // feature, without needing the patch above. However this specific caps
    // feature is going to be removed from GStreamer so it is considered a
    // short-term workaround. This code path most likely will have a negative
    // performance impact on embedded platforms as well. Downstream embedders
    // are highly encouraged to cherry-pick the patch linked above in their BSP
    // and set the WEBKIT_GST_NO_RGBA_CONVERSION environment variable until
    // GStreamer 1.16.2 is released.
    // See also https://bugs.webkit.org/show_bug.cgi?id=201422
    GRefPtr<GstCaps> caps;
    if (webkitGstCheckVersion(1, 16, 2) || getenv("WEBKIT_GST_NO_RGBA_CONVERSION"))
        caps = adoptGRef(gst_caps_from_string("video/x-raw, format = (string) " GST_GL_CAPS_FORMAT));
    else {
        GST_INFO_OBJECT(pipeline(), "Forcing RGBA as GStreamer is not new enough.");
        caps = adoptGRef(gst_caps_from_string("video/x-raw, format = (string) RGBA"));
    }
    gst_caps_set_features(caps.get(), 0, gst_caps_features_new(GST_CAPS_FEATURE_MEMORY_GL_MEMORY, nullptr));
    g_object_set(appsink, "caps", caps.get(), nullptr);

    result &= gst_element_link_many(upload, colorconvert, appsink, nullptr);

    GRefPtr<GstPad> pad = adoptGRef(gst_element_get_static_pad(upload, "sink"));
    gst_element_add_pad(videoSink, gst_ghost_pad_new("sink", pad.get()));

    if (!result) {
        GST_WARNING("Failed to link GstGL elements");
        gst_object_unref(videoSink);
        videoSink = nullptr;
    }
    return videoSink;
}

void MediaPlayerPrivateGStreamer::ensureGLVideoSinkContext()
{
    if (!m_glDisplayElementContext)
        m_glDisplayElementContext = adoptGRef(requestGLContext(GST_GL_DISPLAY_CONTEXT_TYPE));

    if (m_glDisplayElementContext)
        gst_element_set_context(m_videoSink.get(), m_glDisplayElementContext.get());

    if (!m_glAppElementContext)
        m_glAppElementContext = adoptGRef(requestGLContext("gst.gl.app_context"));

    if (m_glAppElementContext)
        gst_element_set_context(m_videoSink.get(), m_glAppElementContext.get());
}
#endif // USE(GSTREAMER_GL)

#if USE(GSTREAMER_HOLEPUNCH)
static void setRectangleToVideoSink(GstElement* videoSink, const IntRect& rect)
{
    // Here goes the platform-dependant code to set to the videoSink the size
    // and position of the video rendering window. Mark them unused as default.
    UNUSED_PARAM(videoSink);
    UNUSED_PARAM(rect);
}

class GStreamerHolePunchClient : public TextureMapperPlatformLayerBuffer::HolePunchClient {
public:
    GStreamerHolePunchClient(GRefPtr<GstElement>&& videoSink) : m_videoSink(WTFMove(videoSink)) { };
    void setVideoRectangle(const IntRect& rect) final { setRectangleToVideoSink(m_videoSink.get(), rect); }
private:
    GRefPtr<GstElement> m_videoSink;
};

GstElement* MediaPlayerPrivateGStreamer::createHolePunchVideoSink()
{
    // Here goes the platform-dependant code to create the videoSink. As a default
    // we use a fakeVideoSink so nothing is drawn to the page.
    GstElement* videoSink =  gst_element_factory_make("fakevideosink", nullptr);

    return videoSink;
}

void MediaPlayerPrivateGStreamer::pushNextHolePunchBuffer()
{
    auto proxyOperation =
        [this](TextureMapperPlatformLayerProxy& proxy)
        {
            LockHolder holder(proxy.lock());
            std::unique_ptr<TextureMapperPlatformLayerBuffer> layerBuffer = makeUnique<TextureMapperPlatformLayerBuffer>(0, m_size, TextureMapperGL::ShouldNotBlend, GL_DONT_CARE);
            std::unique_ptr<GStreamerHolePunchClient> holePunchClient = makeUnique<GStreamerHolePunchClient>(m_videoSink.get());
            layerBuffer->setHolePunchClient(WTFMove(holePunchClient));
            proxy.pushNextBuffer(WTFMove(layerBuffer));
        };

#if USE(NICOSIA)
    proxyOperation(downcast<Nicosia::ContentLayerTextureMapperImpl>(m_nicosiaLayer->impl()).proxy());
#else
    proxyOperation(*m_platformLayerProxy);
#endif
}
#endif

GstElement* MediaPlayerPrivateGStreamer::createVideoSink()
{
    acceleratedRenderingStateChanged();

#if USE(GSTREAMER_HOLEPUNCH)
    m_videoSink = createHolePunchVideoSink();
    pushNextHolePunchBuffer();
    return m_videoSink.get();
#endif

#if USE(GSTREAMER_GL)
    if (m_renderingCanBeAccelerated)
        m_videoSink = createVideoSinkGL();
#endif

    if (!m_videoSink) {
        m_usingFallbackVideoSink = true;
        m_videoSink = webkitVideoSinkNew();
        g_signal_connect_swapped(m_videoSink.get(), "repaint-requested", G_CALLBACK(repaintCallback), this);
        g_signal_connect_swapped(m_videoSink.get(), "repaint-cancelled", G_CALLBACK(repaintCancelledCallback), this);
    }

    GstElement* videoSink = nullptr;
#if ENABLE(MEDIA_STATISTICS)
    m_fpsSink = gst_element_factory_make("fpsdisplaysink", "sink");
    if (m_fpsSink) {
        g_object_set(m_fpsSink.get(), "silent", TRUE , nullptr);

        // Turn off text overlay unless tracing is enabled.
        if (gst_debug_category_get_threshold(webkit_media_player_debug) < GST_LEVEL_TRACE)
            g_object_set(m_fpsSink.get(), "text-overlay", FALSE , nullptr);

        if (g_object_class_find_property(G_OBJECT_GET_CLASS(m_fpsSink.get()), "video-sink")) {
            g_object_set(m_fpsSink.get(), "video-sink", m_videoSink.get(), nullptr);
            videoSink = m_fpsSink.get();
        } else
            m_fpsSink = nullptr;
    }
#endif

    if (!m_fpsSink)
        videoSink = m_videoSink.get();

    ASSERT(videoSink);

    return videoSink;
}

void MediaPlayerPrivateGStreamer::setStreamVolumeElement(GstStreamVolume* volume)
{
    ASSERT(!m_volumeElement);
    m_volumeElement = volume;

    // We don't set the initial volume because we trust the sink to keep it for us. See
    // https://bugs.webkit.org/show_bug.cgi?id=118974 for more information.
    if (!m_player->platformVolumeConfigurationRequired()) {
        GST_DEBUG_OBJECT(pipeline(), "Setting stream volume to %f", m_player->volume());
        gst_stream_volume_set_volume(m_volumeElement.get(), GST_STREAM_VOLUME_FORMAT_LINEAR, static_cast<double>(m_player->volume()));
    } else
        GST_DEBUG_OBJECT(pipeline(), "Not setting stream volume, trusting system one");

    GST_DEBUG_OBJECT(pipeline(), "Setting stream muted %s", toString(m_player->muted()).utf8().data());
    g_object_set(m_volumeElement.get(), "mute", m_player->muted(), nullptr);

    g_signal_connect_swapped(m_volumeElement.get(), "notify::volume", G_CALLBACK(volumeChangedCallback), this);
    g_signal_connect_swapped(m_volumeElement.get(), "notify::mute", G_CALLBACK(muteChangedCallback), this);
}

unsigned MediaPlayerPrivateGStreamer::decodedFrameCount() const
{
    guint64 decodedFrames = 0;
    if (m_fpsSink)
        g_object_get(m_fpsSink.get(), "frames-rendered", &decodedFrames, nullptr);
    return static_cast<unsigned>(decodedFrames);
}

unsigned MediaPlayerPrivateGStreamer::droppedFrameCount() const
{
    guint64 framesDropped = 0;
    if (m_fpsSink)
        g_object_get(m_fpsSink.get(), "frames-dropped", &framesDropped, nullptr);
    return static_cast<unsigned>(framesDropped);
}

unsigned MediaPlayerPrivateGStreamer::audioDecodedByteCount() const
{
    GstQuery* query = gst_query_new_position(GST_FORMAT_BYTES);
    gint64 position = 0;

    if (audioSink() && gst_element_query(audioSink(), query))
        gst_query_parse_position(query, 0, &position);

    gst_query_unref(query);
    return static_cast<unsigned>(position);
}

unsigned MediaPlayerPrivateGStreamer::videoDecodedByteCount() const
{
    GstQuery* query = gst_query_new_position(GST_FORMAT_BYTES);
    gint64 position = 0;

    if (gst_element_query(m_videoSink.get(), query))
        gst_query_parse_position(query, 0, &position);

    gst_query_unref(query);
    return static_cast<unsigned>(position);
}

#if ENABLE(ENCRYPTED_MEDIA)
void MediaPlayerPrivateGStreamer::initializationDataEncountered(InitData&& initData)
{
    ASSERT(!isMainThread());

    RunLoop::main().dispatch([weakThis = makeWeakPtr(*this), initData = WTFMove(initData)] {
        if (!weakThis)
            return;

        GST_DEBUG("scheduling initializationDataEncountered event of size %zu", initData.payload()->size());
        GST_MEMDUMP("init datas", reinterpret_cast<const uint8_t*>(initData.payload()->data()), initData.payload()->size());
        weakThis->m_player->initializationDataEncountered(initData.payloadContainerType(), initData.payload()->tryCreateArrayBuffer());
    });
}

void MediaPlayerPrivateGStreamer::cdmInstanceAttached(CDMInstance& instance)
{
    ASSERT(isMainThread());

    if (m_cdmInstance == &instance)
        return;

    if (!m_pipeline) {
        GST_ERROR("no pipeline yet");
        ASSERT_NOT_REACHED();
        return;
    }

    m_cdmInstance = &instance;

    GRefPtr<GstContext> context = adoptGRef(gst_context_new("drm-cdm-instance", FALSE));
    GstStructure* contextStructure = gst_context_writable_structure(context.get());
    gst_structure_set(contextStructure, "cdm-instance", G_TYPE_POINTER, m_cdmInstance->proxyCDM().get(), nullptr);
    gst_element_set_context(GST_ELEMENT(m_pipeline.get()), context.get());

    GST_DEBUG_OBJECT(m_pipeline.get(), "CDM proxy instance %p dispatched as context", m_cdmInstance->proxyCDM().get());

    m_cdmAttachmentSemaphore.signal();
}

void MediaPlayerPrivateGStreamer::cdmInstanceDetached(CDMInstance& instance)
{
    ASSERT(isMainThread());

    if (m_cdmInstance != &instance) {
        GST_WARNING("passed CDMInstance %p is different from stored one %p", &instance, m_cdmInstance.get());
        ASSERT_NOT_REACHED();
        return;
    }

    ASSERT(m_pipeline);

    GST_DEBUG_OBJECT(m_pipeline.get(), "detaching CDM instance %p, setting empty context", m_cdmInstance.get());
    m_cdmInstance = nullptr;

    GRefPtr<GstContext> context = adoptGRef(gst_context_new("drm-cdm-instance", FALSE));
    gst_element_set_context(GST_ELEMENT(m_pipeline.get()), context.get());
}

void MediaPlayerPrivateGStreamer::attemptToDecryptWithInstance(CDMInstance& instance)
{
    ASSERT(m_cdmInstance.get() == &instance);
    GST_TRACE("instance %p, current stored %p", &instance, m_cdmInstance.get());
    attemptToDecryptWithLocalInstance();
}

void MediaPlayerPrivateGStreamer::attemptToDecryptWithLocalInstance()
{
    bool eventHandled = gst_element_send_event(pipeline(), gst_event_new_custom(GST_EVENT_CUSTOM_DOWNSTREAM_OOB, gst_structure_new_empty("attempt-to-decrypt")));
    GST_DEBUG("attempting to decrypt, event handled %s", boolForPrinting(eventHandled));
}

void MediaPlayerPrivateGStreamer::handleProtectionEvent(GstEvent* event)
{
    {
        LockHolder lock(m_protectionMutex);
        if (m_handledProtectionEvents.contains(GST_EVENT_SEQNUM(event))) {
            GST_DEBUG_OBJECT(pipeline(), "event %u already handled", GST_EVENT_SEQNUM(event));
            return;
        }
    }
    GST_DEBUG_OBJECT(pipeline(), "handling event %u from MSE", GST_EVENT_SEQNUM(event));
    const char* eventKeySystemUUID = nullptr;
    GstBuffer* initData = nullptr;
    gst_event_parse_protection(event, &eventKeySystemUUID, &initData, nullptr);
    initializationDataEncountered({eventKeySystemUUID, initData});
}

void MediaPlayerPrivateGStreamer::setWaitingForKey(bool waitingForKey)
{
    // We bail out if values did not change or if we are requested to not wait anymore but there are still waiting decryptors.
    GST_TRACE("waitingForKey %s, m_waitingForKey %s", boolForPrinting(waitingForKey), boolForPrinting(m_waitingForKey));
    if (waitingForKey == m_waitingForKey || (!waitingForKey && this->waitingForKey()))
        return;

    m_waitingForKey = waitingForKey;
    GST_DEBUG("waiting for key changed %s", boolForPrinting(m_waitingForKey));
    m_player->waitingForKeyChanged();
}

bool MediaPlayerPrivateGStreamer::waitingForKey() const
{
    if (!m_pipeline)
        return false;

    GstState state;
    gst_element_get_state(m_pipeline.get(), &state, nullptr, 0);

    bool result = false;
    GRefPtr<GstQuery> query = adoptGRef(gst_query_new_custom(GST_QUERY_CUSTOM, gst_structure_new_empty("any-decryptor-waiting-for-key")));
    if (state >= GST_STATE_PAUSED) {
        result = gst_element_query(m_pipeline.get(), query.get());
        GST_TRACE("query result %s, on %s", boolForPrinting(result), gst_element_state_get_name(state));
    } else if (state >= GST_STATE_READY) {
        // Running a query in the pipeline is easier but it only works when the pipeline is set up and running, otherwise we need to inspect it and ask the decryptors directly.
        GUniquePtr<GstIterator> iterator(gst_bin_iterate_recurse(GST_BIN(m_pipeline.get())));
        GstIteratorResult iteratorResult;
        do {
            iteratorResult = gst_iterator_fold(iterator.get(), [](const GValue *item, GValue *, gpointer data) -> gboolean {
                GstElement* element = GST_ELEMENT(g_value_get_object(item));
                GstQuery* query = GST_QUERY(data);
                return !WEBKIT_IS_MEDIA_CENC_DECRYPT(element) || !gst_element_query(element, query);
            }, nullptr, query.get());
            if (iteratorResult == GST_ITERATOR_RESYNC)
                gst_iterator_resync(iterator.get());
        } while (iteratorResult == GST_ITERATOR_RESYNC);
        if (iteratorResult == GST_ITERATOR_ERROR)
            GST_WARNING("iterator returned an error");
        result = iteratorResult == GST_ITERATOR_OK;
        GST_TRACE("iterator result %d, waiting %s", iteratorResult, boolForPrinting(result));
    }

    return result;
}
#endif

bool MediaPlayerPrivateGStreamer::supportsKeySystem(const String& keySystem, const String& mimeType)
{
    bool result = false;

#if ENABLE(ENCRYPTED_MEDIA)
    result = GStreamerEMEUtilities::isClearKeyKeySystem(keySystem);
#endif

    GST_DEBUG("checking for KeySystem support with %s and type %s: %s", keySystem.utf8().data(), mimeType.utf8().data(), boolForPrinting(result));
    return result;
}

MediaPlayer::SupportsType MediaPlayerPrivateGStreamer::extendedSupportsType(const MediaEngineSupportParameters& parameters, MediaPlayer::SupportsType result)
{
    UNUSED_PARAM(parameters);
    return result;
}

}

#endif // USE(GSTREAMER)
