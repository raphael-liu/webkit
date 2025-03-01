/*
 * Copyright (C) 2018 Apple Inc. All rights reserved.
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
#include "InlineFormattingContext.h"

#if ENABLE(LAYOUT_FORMATTING_CONTEXT)

#include "FormattingContext.h"
#include "LayoutBox.h"
#include "LayoutContainer.h"

namespace WebCore {
namespace Layout {

ContentWidthAndMargin InlineFormattingContext::Geometry::inlineBlockWidthAndMargin(const Box& formattingContextRoot, const UsedHorizontalValues& usedHorizontalValues)
{
    ASSERT(formattingContextRoot.isInFlow());

    // 10.3.10 'Inline-block', replaced elements in normal flow

    // Exactly as inline replaced elements.
    if (formattingContextRoot.replaced())
        return inlineReplacedWidthAndMargin(formattingContextRoot, usedHorizontalValues);

    // 10.3.9 'Inline-block', non-replaced elements in normal flow

    // If 'width' is 'auto', the used value is the shrink-to-fit width as for floating elements.
    // A computed value of 'auto' for 'margin-left' or 'margin-right' becomes a used value of '0'.
    // #1
    auto width = computedValueIfNotAuto(formattingContextRoot.style().logicalWidth(), usedHorizontalValues.constraints.width);
    if (!width)
        width = shrinkToFitWidth(formattingContextRoot, usedHorizontalValues.constraints.width);

    // #2
    auto computedHorizontalMargin = Geometry::computedHorizontalMargin(formattingContextRoot, usedHorizontalValues);

    return ContentWidthAndMargin { *width, { computedHorizontalMargin.start.valueOr(0_lu), computedHorizontalMargin.end.valueOr(0_lu) }, computedHorizontalMargin };
}

ContentHeightAndMargin InlineFormattingContext::Geometry::inlineBlockHeightAndMargin(const Box& layoutBox, const UsedHorizontalValues& usedHorizontalValues, const UsedVerticalValues& usedVerticalValues) const
{
    ASSERT(layoutBox.isInFlow());

    // 10.6.2 Inline replaced elements, block-level replaced elements in normal flow, 'inline-block' replaced elements in normal flow and floating replaced elements
    if (layoutBox.replaced())
        return inlineReplacedHeightAndMargin(layoutBox, usedHorizontalValues, usedVerticalValues);

    // 10.6.6 Complicated cases
    // - 'Inline-block', non-replaced elements.
    return complicatedCases(layoutBox, usedHorizontalValues, usedVerticalValues);
}

}
}

#endif
