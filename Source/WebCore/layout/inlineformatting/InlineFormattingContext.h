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

#pragma once

#if ENABLE(LAYOUT_FORMATTING_CONTEXT)

#include "FormattingContext.h"
#include "LineLayoutContext.h"
#include <wtf/IsoMalloc.h>

namespace WebCore {
namespace Layout {

class InlineFormattingState;
class InvalidationState;

// This class implements the layout logic for inline formatting contexts.
// https://www.w3.org/TR/CSS22/visuren.html#inline-formatting
class InlineFormattingContext : public FormattingContext {
    WTF_MAKE_ISO_ALLOCATED(InlineFormattingContext);
public:
    InlineFormattingContext(const Container& formattingContextRoot, InlineFormattingState&);
    void layoutInFlowContent(InvalidationState&) override;

private:
    IntrinsicWidthConstraints computedIntrinsicWidthConstraints() override;

    class Quirks : public FormattingContext::Quirks {
    public:
        bool lineDescentNeedsCollapsing(const LineBuilder::RunList&) const;
        LineBuilder::Constraints::HeightAndBaseline lineHeightConstraints(const Box& formattingRoot) const;

    private:
        friend class InlineFormattingContext;
        Quirks(const InlineFormattingContext&);

        const InlineFormattingContext& formattingContext() const { return downcast<InlineFormattingContext>(FormattingContext::Quirks::formattingContext()); }

    };
    InlineFormattingContext::Quirks quirks() const { return Quirks(*this); }

    class Geometry : public FormattingContext::Geometry {
    public:
        ContentHeightAndMargin inlineBlockHeightAndMargin(const Box&, const UsedHorizontalValues&, const UsedVerticalValues&) const;
        ContentWidthAndMargin inlineBlockWidthAndMargin(const Box&, const UsedHorizontalValues&);

    private:
        friend class InlineFormattingContext;
        Geometry(const InlineFormattingContext&);

        const InlineFormattingContext& formattingContext() const { return downcast<InlineFormattingContext>(FormattingContext::Geometry::formattingContext()); }

    };
    InlineFormattingContext::Geometry geometry() const { return Geometry(*this); }

    void lineLayout(const UsedHorizontalValues&);
    void layoutFormattingContextRoot(const Box&, InvalidationState&, const UsedHorizontalValues&, const UsedVerticalValues&);
    void computeHorizontalAndVerticalGeometry(const Box&, const UsedHorizontalValues&, const UsedVerticalValues&);

    void computeIntrinsicWidthForFormattingRoot(const Box&, const UsedHorizontalValues&);
    void computeWidthAndHeightForReplacedInlineBox(const Box&, const UsedHorizontalValues&, const UsedVerticalValues&);
    LayoutUnit computedIntrinsicWidthForConstraint(const UsedHorizontalValues&) const;

    void computeHorizontalMargin(const Box&, const UsedHorizontalValues&);
    void computeHeightAndMargin(const Box&, const UsedHorizontalValues&, const UsedVerticalValues&);
    void computeWidthAndMargin(const Box&, const UsedHorizontalValues&);

    void collectInlineContentIfNeeded();
    LineBuilder::Constraints constraintsForLine(const UsedHorizontalValues&, const LayoutUnit lineLogicalTop);
    void setDisplayBoxesForLine(const LineLayoutContext::LineContent&, const UsedHorizontalValues&);
    void invalidateFormattingState(const InvalidationState&);

    const InlineFormattingState& formattingState() const { return downcast<InlineFormattingState>(FormattingContext::formattingState()); }
    InlineFormattingState& formattingState() { return downcast<InlineFormattingState>(FormattingContext::formattingState()); }
    // FIXME: Come up with a structure that requires no friending.
    friend class LineBuilder;
};

inline InlineFormattingContext::Geometry::Geometry(const InlineFormattingContext& inlineFormattingContext)
    : FormattingContext::Geometry(inlineFormattingContext)
{
}

inline InlineFormattingContext::Quirks::Quirks(const InlineFormattingContext& inlineFormattingContext)
    : FormattingContext::Quirks(inlineFormattingContext)
{
}

}
}

SPECIALIZE_TYPE_TRAITS_LAYOUT_FORMATTING_CONTEXT(InlineFormattingContext, isInlineFormattingContext())

#endif
