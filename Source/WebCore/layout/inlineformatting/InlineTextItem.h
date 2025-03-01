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

#include "InlineFormattingState.h"
#include "InlineItem.h"

namespace WebCore {
namespace Layout {

class InlineTextItem : public InlineItem {
public:
    static void createAndAppendTextItems(InlineItems&, const Box&);

    static std::unique_ptr<InlineTextItem> createWhitespaceItem(const Box&, unsigned start, unsigned length, Optional<LayoutUnit> width);
    static std::unique_ptr<InlineTextItem> createNonWhitespaceItem(const Box&, unsigned start, unsigned length, Optional<LayoutUnit> width);
    static std::unique_ptr<InlineTextItem> createSegmentBreakItem(const Box&, unsigned position);
    static std::unique_ptr<InlineTextItem> createEmptyItem(const Box&);

    unsigned start() const { return m_start; }
    unsigned end() const { return start() + length(); }
    unsigned length() const { return m_length; }

    bool isWhitespace() const { return m_textItemType == TextItemType::Whitespace || isSegmentBreak(); }
    bool isCollapsible() const { return isWhitespace() && style().collapseWhiteSpace(); }
    bool isSegmentBreak() const { return m_textItemType == TextItemType::SegmentBreak; }
    Optional<LayoutUnit> width() const { return m_width; }

    std::unique_ptr<InlineTextItem> left(unsigned length) const;
    std::unique_ptr<InlineTextItem> right(unsigned length) const;

    enum class TextItemType { Undefined, Whitespace, NonWhitespace, SegmentBreak };
    InlineTextItem(const Box&, unsigned start, unsigned length, Optional<LayoutUnit> width, TextItemType);
    InlineTextItem(const Box&);

private:
    unsigned m_start { 0 };
    unsigned m_length { 0 };
    Optional<LayoutUnit> m_width;
    TextItemType m_textItemType { TextItemType::Undefined };
};

}
}

SPECIALIZE_TYPE_TRAITS_INLINE_ITEM(InlineTextItem, isText())

#endif
