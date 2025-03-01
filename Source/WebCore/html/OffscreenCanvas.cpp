/*
 * Copyright (C) 2017 Apple Inc. All rights reserved.
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

#include "config.h"
#include "OffscreenCanvas.h"

#if ENABLE(OFFSCREEN_CANVAS)

#include "CSSValuePool.h"
#include "CanvasRenderingContext.h"
#include "ImageBitmap.h"
#include "JSDOMPromiseDeferred.h"
#include "OffscreenCanvasRenderingContext2D.h"
#include "WebGLRenderingContext.h"
#include "WorkerGlobalScope.h"
#include <wtf/IsoMallocInlines.h>

namespace WebCore {

WTF_MAKE_ISO_ALLOCATED_IMPL(OffscreenCanvas);

Ref<OffscreenCanvas> OffscreenCanvas::create(ScriptExecutionContext& context, unsigned width, unsigned height)
{
    return adoptRef(*new OffscreenCanvas(context, width, height));
}

OffscreenCanvas::OffscreenCanvas(ScriptExecutionContext& context, unsigned width, unsigned height)
    : CanvasBase(IntSize(width, height))
    , ContextDestructionObserver(&context)
{
}

OffscreenCanvas::~OffscreenCanvas()
{
    notifyObserversCanvasDestroyed();

    m_context = nullptr; // Ensure this goes away before the ImageBuffer.
    setImageBuffer(nullptr);
}

void OffscreenCanvas::setWidth(unsigned newWidth)
{
    setSize(IntSize(newWidth, height()));
}

void OffscreenCanvas::setHeight(unsigned newHeight)
{
    setSize(IntSize(width(), newHeight));
}

void OffscreenCanvas::setSize(const IntSize& newSize)
{
    CanvasBase::setSize(newSize);
    reset();
}

ExceptionOr<OffscreenRenderingContext> OffscreenCanvas::getContext(JSC::JSGlobalObject& state, RenderingContextType contextType, Vector<JSC::Strong<JSC::Unknown>>&& arguments)
{
    if (contextType == RenderingContextType::_2d) {
        if (m_context) {
            if (!is<OffscreenCanvasRenderingContext2D>(*m_context))
                return Exception { InvalidStateError };
            return { RefPtr<OffscreenCanvasRenderingContext2D> { &downcast<OffscreenCanvasRenderingContext2D>(*m_context) } };
        }

        m_context = makeUnique<OffscreenCanvasRenderingContext2D>(*this);
        if (!m_context)
            return { RefPtr<OffscreenCanvasRenderingContext2D> { nullptr } };

        return { RefPtr<OffscreenCanvasRenderingContext2D> { &downcast<OffscreenCanvasRenderingContext2D>(*m_context) } };
    }
#if ENABLE(WEBGL)
    if (contextType == RenderingContextType::Webgl) {
        if (m_context) {
            if (!is<WebGLRenderingContext>(*m_context))
                return Exception { InvalidStateError };
            return { RefPtr<WebGLRenderingContext> { &downcast<WebGLRenderingContext>(*m_context) } };
        }

        auto scope = DECLARE_THROW_SCOPE(state.vm());
        auto attributes = convert<IDLDictionary<WebGLContextAttributes>>(state, !arguments.isEmpty() ? arguments[0].get() : JSC::jsUndefined());
        RETURN_IF_EXCEPTION(scope, Exception { ExistingExceptionError });

        m_context = WebGLRenderingContextBase::create(*this, attributes, "webgl");
        if (!m_context)
            return { RefPtr<WebGLRenderingContext> { nullptr } };

        return { RefPtr<WebGLRenderingContext> { &downcast<WebGLRenderingContext>(*m_context) } };
    }
#endif

    return Exception { NotSupportedError };
}

RefPtr<ImageBitmap> OffscreenCanvas::transferToImageBitmap()
{
    if (!m_context)
        return nullptr;

#if ENABLE(WEBGL)
    if (!is<WebGLRenderingContext>(*m_context))
        return nullptr;

    auto webGLContext = &downcast<WebGLRenderingContext>(*m_context);

    // FIXME: We're supposed to create an ImageBitmap using the backing
    // store from this canvas (or its context), but for now we'll just
    // create a new bitmap and paint into it.

    auto imageBitmap = ImageBitmap::create(size());
    if (!imageBitmap->buffer())
        return nullptr;

    auto* gc3d = webGLContext->graphicsContext3D();
    gc3d->paintRenderingResultsToCanvas(imageBitmap->buffer());

    // FIXME: The transfer algorithm requires that the canvas effectively
    // creates a new backing store. Since we're not doing that yet, we
    // need to erase what's there.

    GC3Dfloat clearColor[4];
    gc3d->getFloatv(GraphicsContext3D::COLOR_CLEAR_VALUE, clearColor);
    gc3d->clearColor(0, 0, 0, 0);
    gc3d->clear(GraphicsContext3D::COLOR_BUFFER_BIT | GraphicsContext3D::DEPTH_BUFFER_BIT | GraphicsContext3D::STENCIL_BUFFER_BIT);
    gc3d->clearColor(clearColor[0], clearColor[1], clearColor[2], clearColor[3]);

    return imageBitmap;
#else
    return nullptr;
#endif
}

void OffscreenCanvas::didDraw(const FloatRect& rect)
{
    notifyObserversCanvasChanged(rect);
}

CSSValuePool& OffscreenCanvas::cssValuePool()
{
    auto* context = canvasBaseScriptExecutionContext();
    if (context->isWorkerGlobalScope())
        return downcast<WorkerGlobalScope>(*context).cssValuePool();

    ASSERT(context->isDocument());
    return CSSValuePool::singleton();
}

void OffscreenCanvas::createImageBuffer() const
{
    m_hasCreatedImageBuffer = true;

    if (!width() || !height())
        return;

    setImageBuffer(ImageBuffer::create(size(), Unaccelerated));
}

void OffscreenCanvas::reset()
{
    resetGraphicsContextState();
    if (is<OffscreenCanvasRenderingContext2D>(m_context.get()))
        downcast<OffscreenCanvasRenderingContext2D>(*m_context).reset();

    m_hasCreatedImageBuffer = false;
    setImageBuffer(nullptr);

    notifyObserversCanvasResized();
}

}

#endif
