/*
 * Copyright (C) 1999 Lars Knoll (knoll@kde.org)
 * Copyright (C) 2004-2005 Allan Sandfeld Jensen (kde@carewolf.com)
 * Copyright (C) 2006, 2007 Nicholas Shanks (webkit@nickshanks.com)
 * Copyright (C) 2005-2019 Apple Inc. All rights reserved.
 * Copyright (C) 2007 Alexey Proskuryakov <ap@webkit.org>
 * Copyright (C) 2007, 2008 Eric Seidel <eric@webkit.org>
 * Copyright (C) 2008, 2009 Torch Mobile Inc. All rights reserved. (http://www.torchmobile.com/)
 * Copyright (c) 2011, Code Aurora Forum. All rights reserved.
 * Copyright (C) Research In Motion Limited 2011. All rights reserved.
 * Copyright (C) 2012, 2013 Google Inc. All rights reserved.
 * Copyright (C) 2014 Igalia S.L.
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
 * along with this library; see the file COPYING.LIB.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#include "config.h"
#include "StyleBuilderState.h"

#include "CSSCursorImageValue.h"
#include "CSSFilterImageValue.h"
#include "CSSFontSelector.h"
#include "CSSFunctionValue.h"
#include "CSSGradientValue.h"
#include "CSSImageSetValue.h"
#include "CSSImageValue.h"
#include "CSSShadowValue.h"
#include "RenderTheme.h"
#include "SVGElement.h"
#include "SVGSVGElement.h"
#include "Settings.h"
#include "StyleBuilder.h"
#include "StyleCachedImage.h"
#include "StyleFontSizeFunctions.h"
#include "StyleGeneratedImage.h"
#include "TransformFunctions.h"

namespace WebCore {
namespace Style {

BuilderState::BuilderState(Builder& builder, RenderStyle& style, BuilderContext&& context)
    : m_builder(builder)
    , m_styleMap(*this)
    , m_style(style)
    , m_context(WTFMove(context))
    , m_cssToLengthConversionData(&style, rootElementStyle(), document().renderView())
{
}

// SVG handles zooming in a different way compared to CSS. The whole document is scaled instead
// of each individual length value in the render style / tree. CSSPrimitiveValue::computeLength*()
// multiplies each resolved length with the zoom multiplier - so for SVG we need to disable that.
// Though all CSS values that can be applied to outermost <svg> elements (width/height/border/padding...)
// need to respect the scaling. RenderBox (the parent class of RenderSVGRoot) grabs values like
// width/height/border/padding/... from the RenderStyle -> for SVG these values would never scale,
// if we'd pass a 1.0 zoom factor everyhwere. So we only pass a zoom factor of 1.0 for specific
// properties that are NOT allowed to scale within a zoomed SVG document (letter/word-spacing/font-size).
bool BuilderState::useSVGZoomRules() const
{
    return is<SVGElement>(element());
}

bool BuilderState::useSVGZoomRulesForLength() const
{
    return is<SVGElement>(element()) && !(is<SVGSVGElement>(*element()) && element()->parentNode());
}

RefPtr<StyleImage> BuilderState::createStyleImage(CSSValue& value)
{
    if (is<CSSImageGeneratorValue>(value)) {
        if (is<CSSGradientValue>(value))
            return StyleGeneratedImage::create(downcast<CSSGradientValue>(value).gradientWithStylesResolved(*this));

        if (is<CSSFilterImageValue>(value)) {
            // FilterImage needs to calculate FilterOperations.
            downcast<CSSFilterImageValue>(value).createFilterOperations(*this);
        }
        return StyleGeneratedImage::create(downcast<CSSImageGeneratorValue>(value));
    }

    if (is<CSSImageValue>(value) || is<CSSImageSetValue>(value) || is<CSSCursorImageValue>(value))
        return StyleCachedImage::create(value);

    return nullptr;
}

static FilterOperation::OperationType filterOperationForType(CSSValueID type)
{
    switch (type) {
    case CSSValueUrl:
        return FilterOperation::REFERENCE;
    case CSSValueGrayscale:
        return FilterOperation::GRAYSCALE;
    case CSSValueSepia:
        return FilterOperation::SEPIA;
    case CSSValueSaturate:
        return FilterOperation::SATURATE;
    case CSSValueHueRotate:
        return FilterOperation::HUE_ROTATE;
    case CSSValueInvert:
        return FilterOperation::INVERT;
    case CSSValueAppleInvertLightness:
        return FilterOperation::APPLE_INVERT_LIGHTNESS;
    case CSSValueOpacity:
        return FilterOperation::OPACITY;
    case CSSValueBrightness:
        return FilterOperation::BRIGHTNESS;
    case CSSValueContrast:
        return FilterOperation::CONTRAST;
    case CSSValueBlur:
        return FilterOperation::BLUR;
    case CSSValueDropShadow:
        return FilterOperation::DROP_SHADOW;
    default:
        break;
    }
    ASSERT_NOT_REACHED();
    return FilterOperation::NONE;
}

bool BuilderState::createFilterOperations(const CSSValue& inValue, FilterOperations& outOperations)
{
    // FIXME: Move this code somewhere else.

    ASSERT(outOperations.isEmpty());

    if (is<CSSPrimitiveValue>(inValue)) {
        auto& primitiveValue = downcast<CSSPrimitiveValue>(inValue);
        if (primitiveValue.valueID() == CSSValueNone)
            return true;
    }

    if (!is<CSSValueList>(inValue))
        return false;

    FilterOperations operations;
    for (auto& currentValue : downcast<CSSValueList>(inValue)) {

        if (is<CSSPrimitiveValue>(currentValue)) {
            auto& primitiveValue = downcast<CSSPrimitiveValue>(currentValue.get());
            if (!primitiveValue.isURI())
                continue;

            String cssUrl = primitiveValue.stringValue();
            URL url = document().completeURL(cssUrl);

            auto operation = ReferenceFilterOperation::create(cssUrl, url.fragmentIdentifier());
            operations.operations().append(WTFMove(operation));
            continue;
        }

        if (!is<CSSFunctionValue>(currentValue))
            continue;

        auto& filterValue = downcast<CSSFunctionValue>(currentValue.get());
        FilterOperation::OperationType operationType = filterOperationForType(filterValue.name());

        // Check that all parameters are primitive values, with the
        // exception of drop shadow which has a CSSShadowValue parameter.
        const CSSPrimitiveValue* firstValue = nullptr;
        if (operationType != FilterOperation::DROP_SHADOW) {
            bool haveNonPrimitiveValue = false;
            for (unsigned j = 0; j < filterValue.length(); ++j) {
                if (!is<CSSPrimitiveValue>(*filterValue.itemWithoutBoundsCheck(j))) {
                    haveNonPrimitiveValue = true;
                    break;
                }
            }
            if (haveNonPrimitiveValue)
                continue;
            if (filterValue.length())
                firstValue = downcast<CSSPrimitiveValue>(filterValue.itemWithoutBoundsCheck(0));
        }

        switch (operationType) {
        case FilterOperation::GRAYSCALE:
        case FilterOperation::SEPIA:
        case FilterOperation::SATURATE: {
            double amount = 1;
            if (filterValue.length() == 1) {
                amount = firstValue->doubleValue();
                if (firstValue->isPercentage())
                    amount /= 100;
            }

            operations.operations().append(BasicColorMatrixFilterOperation::create(amount, operationType));
            break;
        }
        case FilterOperation::HUE_ROTATE: {
            double angle = 0;
            if (filterValue.length() == 1)
                angle = firstValue->computeDegrees();

            operations.operations().append(BasicColorMatrixFilterOperation::create(angle, operationType));
            break;
        }
        case FilterOperation::INVERT:
        case FilterOperation::BRIGHTNESS:
        case FilterOperation::CONTRAST:
        case FilterOperation::OPACITY: {
            double amount = 1;
            if (filterValue.length() == 1) {
                amount = firstValue->doubleValue();
                if (firstValue->isPercentage())
                    amount /= 100;
            }

            operations.operations().append(BasicComponentTransferFilterOperation::create(amount, operationType));
            break;
        }
        case FilterOperation::APPLE_INVERT_LIGHTNESS: {
            operations.operations().append(InvertLightnessFilterOperation::create());
            break;
        }
        case FilterOperation::BLUR: {
            Length stdDeviation = Length(0, Fixed);
            if (filterValue.length() >= 1)
                stdDeviation = convertToFloatLength(firstValue, cssToLengthConversionData());
            if (stdDeviation.isUndefined())
                return false;

            operations.operations().append(BlurFilterOperation::create(stdDeviation));
            break;
        }
        case FilterOperation::DROP_SHADOW: {
            if (filterValue.length() != 1)
                return false;

            const auto* cssValue = filterValue.itemWithoutBoundsCheck(0);
            if (!is<CSSShadowValue>(cssValue))
                continue;

            const auto& item = downcast<CSSShadowValue>(*cssValue);
            int x = item.x->computeLength<int>(cssToLengthConversionData());
            int y = item.y->computeLength<int>(cssToLengthConversionData());
            IntPoint location(x, y);
            int blur = item.blur ? item.blur->computeLength<int>(cssToLengthConversionData()) : 0;
            Color color;
            if (item.color)
                color = colorFromPrimitiveValue(*item.color);

            operations.operations().append(DropShadowFilterOperation::create(location, blur, color.isValid() ? color : Color::transparent));
            break;
        }
        default:
            ASSERT_NOT_REACHED();
            break;
        }
    }

    outOperations = operations;
    return true;
}

bool BuilderState::isColorFromPrimitiveValueDerivedFromElement(const CSSPrimitiveValue& value)
{
    switch (value.valueID()) {
    case CSSValueWebkitText:
    case CSSValueWebkitLink:
    case CSSValueWebkitActivelink:
    case CSSValueCurrentcolor:
        return true;
    default:
        return false;
    }
}

Color BuilderState::colorFromPrimitiveValue(const CSSPrimitiveValue& value, bool forVisitedLink) const
{
    if (value.isRGBColor())
        return value.color();

    auto identifier = value.valueID();
    switch (identifier) {
    case CSSValueWebkitText:
        return document().textColor();
    case CSSValueWebkitLink:
        return (element() && element()->isLink() && forVisitedLink) ? document().visitedLinkColor() : document().linkColor();
    case CSSValueWebkitActivelink:
        return document().activeLinkColor();
    case CSSValueWebkitFocusRingColor:
        return RenderTheme::singleton().focusRingColor(document().styleColorOptions(&m_style));
    case CSSValueCurrentcolor:
        // Color is an inherited property so depending on it effectively makes the property inherited.
        // FIXME: Setting the flag as a side effect of calling this function is a bit oblique. Can we do better?
        m_style.setHasExplicitlyInheritedProperties();
        return m_style.color();
    default:
        return StyleColor::colorFromKeyword(identifier, document().styleColorOptions(&m_style));
    }
}

void BuilderState::registerContentAttribute(const AtomString& attributeLocalName)
{
    if (style().styleType() == PseudoId::Before || style().styleType() == PseudoId::After)
        m_registeredContentAttributes.append(attributeLocalName);
}

void BuilderState::adjustStyleForInterCharacterRuby()
{
    if (m_style.rubyPosition() != RubyPosition::InterCharacter || !element() || !element()->hasTagName(HTMLNames::rtTag))
        return;

    m_style.setTextAlign(TextAlignMode::Center);
    if (m_style.isHorizontalWritingMode())
        m_style.setWritingMode(LeftToRightWritingMode);
}

void BuilderState::updateFont()
{
    if (!m_fontDirty && m_style.fontCascade().fonts())
        return;

#if ENABLE(TEXT_AUTOSIZING)
    updateFontForTextSizeAdjust();
#endif
    updateFontForGenericFamilyChange();
    updateFontForZoomChange();
    updateFontForOrientationChange();

    m_style.fontCascade().update(&const_cast<Document&>(document()).fontSelector());

    m_fontDirty = false;
}

#if ENABLE(TEXT_AUTOSIZING)
void BuilderState::updateFontForTextSizeAdjust()
{
    if (m_style.textSizeAdjust().isAuto()
        || !document().settings().textAutosizingEnabled()
        || (document().settings().textAutosizingUsesIdempotentMode() && !m_style.textSizeAdjust().isNone()))
        return;

    auto newFontDescription = m_style.fontDescription();
    if (!m_style.textSizeAdjust().isNone())
        newFontDescription.setComputedSize(newFontDescription.specifiedSize() * m_style.textSizeAdjust().multiplier());
    else
        newFontDescription.setComputedSize(newFontDescription.specifiedSize());

    m_style.setFontDescription(WTFMove(newFontDescription));
}
#endif

void BuilderState::updateFontForZoomChange()
{
    if (m_style.effectiveZoom() == parentStyle().effectiveZoom() && m_style.textZoom() == parentStyle().textZoom())
        return;

    const auto& childFont = m_style.fontDescription();
    auto newFontDescription = childFont;
    setFontSize(newFontDescription, childFont.specifiedSize());

    m_style.setFontDescription(WTFMove(newFontDescription));
}

void BuilderState::updateFontForGenericFamilyChange()
{
    const auto& childFont = m_style.fontDescription();

    if (childFont.isAbsoluteSize())
        return;

    const auto& parentFont = parentStyle().fontDescription();
    if (childFont.useFixedDefaultSize() == parentFont.useFixedDefaultSize())
        return;

    // We know the parent is monospace or the child is monospace, and that font
    // size was unspecified. We want to scale our font size as appropriate.
    // If the font uses a keyword size, then we refetch from the table rather than
    // multiplying by our scale factor.
    float size = [&] {
        if (CSSValueID sizeIdentifier = childFont.keywordSizeAsIdentifier())
            return Style::fontSizeForKeyword(sizeIdentifier, childFont.useFixedDefaultSize(), document());

        auto fixedSize =  document().settings().defaultFixedFontSize();
        auto defaultSize =  document().settings().defaultFontSize();
        float fixedScaleFactor = (fixedSize && defaultSize) ? static_cast<float>(fixedSize) / defaultSize : 1;
        return parentFont.useFixedDefaultSize() ? childFont.specifiedSize() / fixedScaleFactor : childFont.specifiedSize() * fixedScaleFactor;
    }();

    auto newFontDescription = childFont;
    setFontSize(newFontDescription, size);
    m_style.setFontDescription(WTFMove(newFontDescription));
}

void BuilderState::updateFontForOrientationChange()
{
    auto [fontOrientation, glyphOrientation] = m_style.fontAndGlyphOrientation();

    const auto& fontDescription = m_style.fontDescription();
    if (fontDescription.orientation() == fontOrientation && fontDescription.nonCJKGlyphOrientation() == glyphOrientation)
        return;

    auto newFontDescription = fontDescription;
    newFontDescription.setNonCJKGlyphOrientation(glyphOrientation);
    newFontDescription.setOrientation(fontOrientation);
    m_style.setFontDescription(WTFMove(newFontDescription));
}

void BuilderState::setFontSize(FontCascadeDescription& fontDescription, float size)
{
    fontDescription.setSpecifiedSize(size);
    fontDescription.setComputedSize(Style::computedFontSizeFromSpecifiedSize(size, fontDescription.isAbsoluteSize(), useSVGZoomRules(), &style(), document()));
}

}
}
