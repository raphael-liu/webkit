/*
 * Copyright (C) 2014-2018 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */
#import "config.h"
#import "VideoFullscreenManager.h"

#if (PLATFORM(IOS_FAMILY) && HAVE(AVKIT)) || (PLATFORM(MAC) && ENABLE(VIDEO_PRESENTATION_MODE))

#import "Attachment.h"
#import "LayerHostingContext.h"
#import "Logging.h"
#import "PlaybackSessionManager.h"
#import "VideoFullscreenManagerMessages.h"
#import "VideoFullscreenManagerProxyMessages.h"
#import "WebCoreArgumentCoders.h"
#import "WebPage.h"
#import "WebProcess.h"
#import <QuartzCore/CoreAnimation.h>
#import <WebCore/Color.h>
#import <WebCore/DeprecatedGlobalSettings.h>
#import <WebCore/Event.h>
#import <WebCore/EventNames.h>
#import <WebCore/FrameView.h>
#import <WebCore/HTMLVideoElement.h>
#import <WebCore/PictureInPictureSupport.h>
#import <WebCore/PlatformCALayer.h>
#import <WebCore/RenderLayer.h>
#import <WebCore/RenderLayerBacking.h>
#import <WebCore/RenderVideo.h>
#import <WebCore/RenderView.h>
#import <WebCore/Settings.h>
#import <WebCore/TimeRanges.h>
#import <WebCore/WebActionDisablingCALayerDelegate.h>
#import <mach/mach_port.h>
#import <wtf/MachSendRight.h>

namespace WebKit {
using namespace WebCore;

static IntRect inlineVideoFrame(HTMLVideoElement& element)
{
    auto& document = element.document();
    if (!document.hasLivingRenderTree() || document.activeDOMObjectsAreStopped())
        return { };

    document.updateLayoutIgnorePendingStylesheets();
    auto* renderer = element.renderer();
    if (!renderer)
        return { };

    if (renderer->hasLayer() && renderer->enclosingLayer()->isComposited()) {
        FloatQuad contentsBox = static_cast<FloatRect>(renderer->enclosingLayer()->backing()->contentsBox());
        contentsBox = renderer->localToAbsoluteQuad(contentsBox);
        return element.document().view()->contentsToRootView(contentsBox.enclosingBoundingBox());
    }

    auto rect = renderer->videoBox();
    rect.moveBy(renderer->absoluteBoundingBoxRect().location());
    return element.document().view()->contentsToRootView(rect);
}

#pragma mark - VideoFullscreenInterfaceContext

VideoFullscreenInterfaceContext::VideoFullscreenInterfaceContext(VideoFullscreenManager& manager, uint64_t contextId)
    : m_manager(&manager)
    , m_contextId(contextId)
{
}

VideoFullscreenInterfaceContext::~VideoFullscreenInterfaceContext()
{
}

void VideoFullscreenInterfaceContext::setLayerHostingContext(std::unique_ptr<LayerHostingContext>&& context)
{
    m_layerHostingContext = WTFMove(context);
}

void VideoFullscreenInterfaceContext::hasVideoChanged(bool hasVideo)
{
    if (m_manager)
        m_manager->hasVideoChanged(m_contextId, hasVideo);
}

void VideoFullscreenInterfaceContext::videoDimensionsChanged(const FloatSize& videoDimensions)
{
    if (m_manager)
        m_manager->videoDimensionsChanged(m_contextId, videoDimensions);
}

#pragma mark - VideoFullscreenManager

Ref<VideoFullscreenManager> VideoFullscreenManager::create(WebPage& page, PlaybackSessionManager& playbackSessionManager)
{
    return adoptRef(*new VideoFullscreenManager(page, playbackSessionManager));
}

VideoFullscreenManager::VideoFullscreenManager(WebPage& page, PlaybackSessionManager& playbackSessionManager)
    : m_page(&page)
    , m_playbackSessionManager(playbackSessionManager)
{
    WebProcess::singleton().addMessageReceiver(Messages::VideoFullscreenManager::messageReceiverName(), page.identifier(), *this);
}

VideoFullscreenManager::~VideoFullscreenManager()
{
    for (auto& [model, interface] : m_contextMap.values()) {
        model->setVideoElement(nullptr);
        model->removeClient(*interface);

        interface->invalidate();
    }

    m_contextMap.clear();
    m_videoElements.clear();
    m_clientCounts.clear();
    
    if (m_page)
        WebProcess::singleton().removeMessageReceiver(Messages::VideoFullscreenManager::messageReceiverName(), m_page->identifier());
}

void VideoFullscreenManager::invalidate()
{
    ASSERT(m_page);
    WebProcess::singleton().removeMessageReceiver(Messages::VideoFullscreenManager::messageReceiverName(), m_page->identifier());
    m_page = nullptr;
}

VideoFullscreenManager::ModelInterfaceTuple VideoFullscreenManager::createModelAndInterface(uint64_t contextId)
{
    auto model = VideoFullscreenModelVideoElement::create();
    auto interface = VideoFullscreenInterfaceContext::create(*this, contextId);
    m_playbackSessionManager->addClientForContext(contextId);

    interface->setLayerHostingContext(LayerHostingContext::createForExternalHostingProcess());
    model->addClient(interface.get());

    return std::make_tuple(WTFMove(model), WTFMove(interface));
}

VideoFullscreenManager::ModelInterfaceTuple& VideoFullscreenManager::ensureModelAndInterface(uint64_t contextId)
{
    auto addResult = m_contextMap.add(contextId, ModelInterfaceTuple());
    if (addResult.isNewEntry)
        addResult.iterator->value = createModelAndInterface(contextId);
    return addResult.iterator->value;
}

WebCore::VideoFullscreenModelVideoElement& VideoFullscreenManager::ensureModel(uint64_t contextId)
{
    return *std::get<0>(ensureModelAndInterface(contextId));
}

VideoFullscreenInterfaceContext& VideoFullscreenManager::ensureInterface(uint64_t contextId)
{
    return *std::get<1>(ensureModelAndInterface(contextId));
}

void VideoFullscreenManager::removeContext(uint64_t contextId)
{
    auto [model, interface] = ensureModelAndInterface(contextId);

    m_playbackSessionManager->removeClientForContext(contextId);

    RefPtr<HTMLVideoElement> videoElement = model->videoElement();
    model->setVideoElement(nullptr);
    model->removeClient(*interface);
    interface->invalidate();
    m_videoElements.remove(videoElement.get());
    m_contextMap.remove(contextId);
}

void VideoFullscreenManager::addClientForContext(uint64_t contextId)
{
    auto addResult = m_clientCounts.add(contextId, 1);
    if (!addResult.isNewEntry)
        addResult.iterator->value++;
}

void VideoFullscreenManager::removeClientForContext(uint64_t contextId)
{
    ASSERT(m_clientCounts.contains(contextId));

    int clientCount = m_clientCounts.get(contextId);
    ASSERT(clientCount > 0);
    clientCount--;

    if (clientCount <= 0) {
        m_clientCounts.remove(contextId);
        removeContext(contextId);
        return;
    }

    m_clientCounts.set(contextId, clientCount);
}

#pragma mark Interface to ChromeClient:

bool VideoFullscreenManager::supportsVideoFullscreen(WebCore::HTMLMediaElementEnums::VideoFullscreenMode mode) const
{
#if PLATFORM(IOS_FAMILY)
    UNUSED_PARAM(mode);
    return DeprecatedGlobalSettings::avKitEnabled();
#else
    return mode == HTMLMediaElementEnums::VideoFullscreenModePictureInPicture && supportsPictureInPicture();
#endif
}

bool VideoFullscreenManager::supportsVideoFullscreenStandby() const
{
#if PLATFORM(IOS_FAMILY)
    return true;
#else
    return false;
#endif
}

void VideoFullscreenManager::enterVideoFullscreenForVideoElement(HTMLVideoElement& videoElement, HTMLMediaElementEnums::VideoFullscreenMode mode, bool standby)
{
    ASSERT(m_page);
    ASSERT(standby || mode != HTMLMediaElementEnums::VideoFullscreenModeNone);
    LOG(Fullscreen, "VideoFullscreenManager::enterVideoFullscreenForVideoElement(%p)", this);

    uint64_t contextId = m_playbackSessionManager->contextIdForMediaElement(videoElement);
    auto addResult = m_videoElements.add(&videoElement, contextId);
    UNUSED_PARAM(addResult);
    ASSERT(addResult.iterator->value == contextId);

    auto [model, interface] = ensureModelAndInterface(contextId);
    HTMLMediaElementEnums::VideoFullscreenMode oldMode = interface->fullscreenMode();
    if (oldMode == mode)
        return;

    addClientForContext(contextId);
    if (!interface->layerHostingContext())
        interface->setLayerHostingContext(LayerHostingContext::createForExternalHostingProcess());

    auto videoRect = inlineVideoFrame(videoElement);
    FloatRect videoLayerFrame = FloatRect(0, 0, videoRect.width(), videoRect.height());

    interface->setTargetIsFullscreen(true);
    interface->setFullscreenMode(mode);
    interface->setFullscreenStandby(standby);
    model->setVideoElement(&videoElement);
    if (oldMode == HTMLMediaElementEnums::VideoFullscreenModeNone && mode != HTMLMediaElementEnums::VideoFullscreenModeNone)
        model->setVideoLayerFrame(videoLayerFrame);

    if (interface->animationState() != VideoFullscreenInterfaceContext::AnimationType::None)
        return;
    interface->setAnimationState(VideoFullscreenInterfaceContext::AnimationType::IntoFullscreen);

    bool allowsPictureInPicture = videoElement.webkitSupportsPresentationMode(HTMLVideoElement::VideoPresentationMode::PictureInPicture);

    if (!interface->layerHostingContext()->rootLayer()) {
        PlatformLayer* videoLayer = [CALayer layer];
        [videoLayer setDelegate:[WebActionDisablingCALayerDelegate shared]];

        [videoLayer setName:@"Web video fullscreen manager layer"];
        [videoLayer setPosition:CGPointMake(0, 0)];
        [videoLayer setBackgroundColor:cachedCGColor(WebCore::Color::transparent)];

        // Set a scale factor here to make convertRect:toLayer:nil take scale factor into account. <rdar://problem/18316542>.
        // This scale factor is inverted in the hosting process.
        float hostingScaleFactor = m_page->deviceScaleFactor();
        [videoLayer setTransform:CATransform3DMakeScale(hostingScaleFactor, hostingScaleFactor, 1)];

        interface->layerHostingContext()->setRootLayer(videoLayer);
    }

    m_page->send(Messages::VideoFullscreenManagerProxy::SetupFullscreenWithID(contextId, interface->layerHostingContext()->contextID(), videoRect, m_page->deviceScaleFactor(), interface->fullscreenMode(), allowsPictureInPicture, standby));
}

void VideoFullscreenManager::exitVideoFullscreenForVideoElement(WebCore::HTMLVideoElement& videoElement)
{
    LOG(Fullscreen, "VideoFullscreenManager::exitVideoFullscreenForVideoElement(%p)", this);
    ASSERT(m_page);
    ASSERT(m_videoElements.contains(&videoElement));

    uint64_t contextId = m_videoElements.get(&videoElement);
    auto& interface = ensureInterface(contextId);
    HTMLMediaElementEnums::VideoFullscreenMode oldMode = interface.fullscreenMode();
    if (oldMode == HTMLMediaElementEnums::VideoFullscreenModeNone)
        return;

    interface.setTargetIsFullscreen(false);

    if (interface.animationState() != VideoFullscreenInterfaceContext::AnimationType::None)
        return;
    interface.setAnimationState(VideoFullscreenInterfaceContext::AnimationType::FromFullscreen);
    m_page->send(Messages::VideoFullscreenManagerProxy::ExitFullscreen(contextId, inlineVideoFrame(videoElement)));
}

void VideoFullscreenManager::exitVideoFullscreenToModeWithoutAnimation(WebCore::HTMLVideoElement& videoElement, WebCore::HTMLMediaElementEnums::VideoFullscreenMode targetMode)
{
    LOG(Fullscreen, "VideoFullscreenManager::exitVideoFullscreenToModeWithoutAnimation(%p)", this);

#if PLATFORM(MAC) && ENABLE(VIDEO_PRESENTATION_MODE)
    ASSERT(m_page);
    ASSERT(m_videoElements.contains(&videoElement));

    uint64_t contextId = m_videoElements.get(&videoElement);
    auto& interface = ensureInterface(contextId);

    interface.setTargetIsFullscreen(false);

    m_page->send(Messages::VideoFullscreenManagerProxy::ExitFullscreenWithoutAnimationToMode(contextId, targetMode));
#else
    UNUSED_PARAM(videoElement);
    UNUSED_PARAM(targetMode);
#endif
}

#pragma mark Interface to VideoFullscreenInterfaceContext:

void VideoFullscreenManager::hasVideoChanged(uint64_t contextId, bool hasVideo)
{
    if (m_page)
        m_page->send(Messages::VideoFullscreenManagerProxy::SetHasVideo(contextId, hasVideo));
}

void VideoFullscreenManager::videoDimensionsChanged(uint64_t contextId, const FloatSize& videoDimensions)
{
    if (m_page)
        m_page->send(Messages::VideoFullscreenManagerProxy::SetVideoDimensions(contextId, videoDimensions));
}

#pragma mark Messages from VideoFullscreenManagerProxy:

void VideoFullscreenManager::requestFullscreenMode(uint64_t contextId, WebCore::HTMLMediaElementEnums::VideoFullscreenMode mode, bool finishedWithMedia)
{
    ensureModel(contextId).requestFullscreenMode(mode, finishedWithMedia);
}

void VideoFullscreenManager::fullscreenModeChanged(uint64_t contextId, WebCore::HTMLMediaElementEnums::VideoFullscreenMode videoFullscreenMode)
{
    ensureModel(contextId).fullscreenModeChanged(videoFullscreenMode);
}

void VideoFullscreenManager::requestUpdateInlineRect(uint64_t contextId)
{
    if (!m_page)
        return;

    auto& model = ensureModel(contextId);
    IntRect inlineRect = inlineVideoFrame(*model.videoElement());
    m_page->send(Messages::VideoFullscreenManagerProxy::SetInlineRect(contextId, inlineRect, inlineRect != IntRect(0, 0, 0, 0)));
}

void VideoFullscreenManager::requestVideoContentLayer(uint64_t contextId)
{
    auto [model, interface] = ensureModelAndInterface(contextId);

    CALayer* videoLayer = interface->layerHostingContext()->rootLayer();

    model->setVideoFullscreenLayer(videoLayer, [protectedThis = makeRefPtr(this), this, contextId] () mutable {
        dispatch_async(dispatch_get_main_queue(), [protectedThis = WTFMove(protectedThis), this, contextId] {
            if (protectedThis->m_page)
                m_page->send(Messages::VideoFullscreenManagerProxy::SetHasVideoContentLayer(contextId, true));
        });
    });
}

void VideoFullscreenManager::returnVideoContentLayer(uint64_t contextId)
{
    RefPtr<VideoFullscreenModelVideoElement> model;
    RefPtr<VideoFullscreenInterfaceContext> interface;
    std::tie(model, interface) = ensureModelAndInterface(contextId);

    model->waitForPreparedForInlineThen([protectedThis = makeRefPtr(this), this, contextId, model] () mutable { // need this for return video layer
        dispatch_async(dispatch_get_main_queue(), [protectedThis = WTFMove(protectedThis), this, contextId, model] () mutable {
            model->setVideoFullscreenLayer(nil, [protectedThis = WTFMove(protectedThis), this, contextId] () mutable {
                dispatch_async(dispatch_get_main_queue(), [protectedThis = WTFMove(protectedThis), this, contextId] {
                    if (protectedThis->m_page)
                        m_page->send(Messages::VideoFullscreenManagerProxy::SetHasVideoContentLayer(contextId, false));
                });
            });
        });
    });
}

void VideoFullscreenManager::didSetupFullscreen(uint64_t contextId)
{
    LOG(Fullscreen, "VideoFullscreenManager::didSetupFullscreen(%p, %x)", this, contextId);

    ASSERT(m_page);
    auto [model, interface] = ensureModelAndInterface(contextId);

#if PLATFORM(IOS_FAMILY)
    dispatch_async(dispatch_get_main_queue(), [protectedThis = makeRefPtr(this), this, contextId] {
        if (protectedThis->m_page)
            m_page->send(Messages::VideoFullscreenManagerProxy::EnterFullscreen(contextId));
    });
#else
    CALayer* videoLayer = interface->layerHostingContext()->rootLayer();

    model->setVideoFullscreenLayer(videoLayer, [protectedThis = makeRefPtr(this), this, contextId] () mutable {
        dispatch_async(dispatch_get_main_queue(), [protectedThis = WTFMove(protectedThis), this, contextId] {
            if (protectedThis->m_page)
                m_page->send(Messages::VideoFullscreenManagerProxy::EnterFullscreen(contextId));
        });
    });
#endif
}

void VideoFullscreenManager::willExitFullscreen(uint64_t contextId)
{
    LOG(Fullscreen, "VideoFullscreenManager::willExitFullscreen(%p, %x)", this, contextId);

    auto [model, interface] = ensureModelAndInterface(contextId);

    RefPtr<HTMLVideoElement> videoElement = model->videoElement();
    if (!videoElement)
        return;

    dispatch_async(dispatch_get_main_queue(), [protectedThis = makeRefPtr(this), videoElement = WTFMove(videoElement), contextId] {
        videoElement->willExitFullscreen();
        if (protectedThis->m_page)
            protectedThis->m_page->send(Messages::VideoFullscreenManagerProxy::PreparedToExitFullscreen(contextId));
    });
}

void VideoFullscreenManager::didEnterFullscreen(uint64_t contextId)
{
    LOG(Fullscreen, "VideoFullscreenManager::didEnterFullscreen(%p, %x)", this, contextId);

    auto [model, interface] = ensureModelAndInterface(contextId);

    interface->setAnimationState(VideoFullscreenInterfaceContext::AnimationType::None);
    interface->setIsFullscreen(false);

    RefPtr<HTMLVideoElement> videoElement = model->videoElement();
    if (!videoElement)
        return;

    dispatch_async(dispatch_get_main_queue(), [protectedThis = makeRefPtr(this), videoElement] {
        videoElement->didBecomeFullscreenElement();
    });

    if (interface->targetIsFullscreen())
        return;

    // exit fullscreen now if it was previously requested during an animation.
    dispatch_async(dispatch_get_main_queue(), [protectedThis = makeRefPtr(this), videoElement] {
        if (protectedThis->m_page)
            protectedThis->exitVideoFullscreenForVideoElement(*videoElement);
    });
}

void VideoFullscreenManager::didExitFullscreen(uint64_t contextId)
{
    LOG(Fullscreen, "VideoFullscreenManager::didExitFullscreen(%p, %x)", this, contextId);

    RefPtr<VideoFullscreenModelVideoElement> model;
    RefPtr<VideoFullscreenInterfaceContext> interface;
    std::tie(model, interface) = ensureModelAndInterface(contextId);

#if PLATFORM(IOS_FAMILY)
    dispatch_async(dispatch_get_main_queue(), [protectedThis = makeRefPtr(this), contextId, interface] {
        if (protectedThis->m_page)
            protectedThis->m_page->send(Messages::VideoFullscreenManagerProxy::CleanupFullscreen(contextId));
    });
#else
    model->waitForPreparedForInlineThen([protectedThis = makeRefPtr(this), contextId, interface, model] () mutable {
        dispatch_async(dispatch_get_main_queue(), [protectedThis = WTFMove(protectedThis), contextId, interface, model] () mutable {
            model->setVideoFullscreenLayer(nil, [protectedThis = WTFMove(protectedThis), contextId, interface] () mutable {
                dispatch_async(dispatch_get_main_queue(), [protectedThis = WTFMove(protectedThis), contextId, interface] {
                    if (interface->layerHostingContext()) {
                        interface->layerHostingContext()->setRootLayer(nullptr);
                        interface->setLayerHostingContext(nullptr);
                    }
                    if (protectedThis->m_page)
                        protectedThis->m_page->send(Messages::VideoFullscreenManagerProxy::CleanupFullscreen(contextId));
                });
            });
        });
    });
#endif
}
    
void VideoFullscreenManager::didCleanupFullscreen(uint64_t contextId)
{
    LOG(Fullscreen, "VideoFullscreenManager::didCleanupFullscreen(%p, %x)", this, contextId);

    auto [model, interface] = ensureModelAndInterface(contextId);

    if (interface->layerHostingContext()) {
        interface->layerHostingContext()->setRootLayer(nullptr);
        interface->setLayerHostingContext(nullptr);
    }

    interface->setAnimationState(VideoFullscreenInterfaceContext::AnimationType::None);
    interface->setIsFullscreen(false);
    HTMLMediaElementEnums::VideoFullscreenMode mode = interface->fullscreenMode();
    bool standby = interface->fullscreenStandby();
    bool targetIsFullscreen = interface->targetIsFullscreen();

    model->setVideoFullscreenLayer(nil);
    RefPtr<HTMLVideoElement> videoElement = model->videoElement();

    interface->setFullscreenMode(HTMLMediaElementEnums::VideoFullscreenModeNone);
    interface->setFullscreenStandby(false);
    removeClientForContext(contextId);

    if (!videoElement || !targetIsFullscreen)
        return;

    dispatch_async(dispatch_get_main_queue(), [protectedThis = makeRefPtr(this), videoElement, mode, standby] {
        if (protectedThis->m_page)
            protectedThis->enterVideoFullscreenForVideoElement(*videoElement, mode, standby);
    });
}
    
void VideoFullscreenManager::setVideoLayerGravityEnum(uint64_t contextId, unsigned gravity)
{
    ensureModel(contextId).setVideoLayerGravity((MediaPlayerEnums::VideoGravity)gravity);
}
    
void VideoFullscreenManager::fullscreenMayReturnToInline(uint64_t contextId, bool isPageVisible)
{
    if (!m_page)
        return;

    auto& model = ensureModel(contextId);

    if (!isPageVisible)
        model.videoElement()->scrollIntoViewIfNotVisible(false);
    m_page->send(Messages::VideoFullscreenManagerProxy::PreparedToReturnToInline(contextId, true, inlineVideoFrame(*model.videoElement())));
}

void VideoFullscreenManager::requestRouteSharingPolicyAndContextUID(uint64_t contextId, Messages::VideoFullscreenManager::RequestRouteSharingPolicyAndContextUID::AsyncReply&& reply)
{
    ensureModel(contextId).requestRouteSharingPolicyAndContextUID(WTFMove(reply));
}
    
void VideoFullscreenManager::setVideoLayerFrameFenced(uint64_t contextId, WebCore::FloatRect bounds, IPC::Attachment fencePort)
{
    LOG(Fullscreen, "VideoFullscreenManager::setVideoLayerFrameFenced(%p, %x)", this, contextId);

    if (fencePort.disposition() != MACH_MSG_TYPE_MOVE_SEND) {
        LOG(Fullscreen, "VideoFullscreenManager::setVideoLayerFrameFenced(%p, %x) Received an invalid fence port: %d, disposition: %d", this, contextId, fencePort.port(), fencePort.disposition());
        return;
    }

    auto [model, interface] = ensureModelAndInterface(contextId);

    if (std::isnan(bounds.x()) || std::isnan(bounds.y()) || std::isnan(bounds.width()) || std::isnan(bounds.height())) {
        auto videoRect = inlineVideoFrame(*model->videoElement());
        bounds = FloatRect(0, 0, videoRect.width(), videoRect.height());
    }
    
    if (auto* context = interface->layerHostingContext())
        context->setFencePort(fencePort.port());
    model->setVideoLayerFrame(bounds);
    deallocateSendRightSafely(fencePort.port());
}

} // namespace WebKit

#endif // PLATFORM(IOS_FAMILY) || (PLATFORM(MAC) && ENABLE(VIDEO_PRESENTATION_MODE))
