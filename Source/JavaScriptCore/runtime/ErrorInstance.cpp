/*
 *  Copyright (C) 1999-2000 Harri Porten (porten@kde.org)
 *  Copyright (C) 2003-2019 Apple Inc. All rights reserved.
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#include "config.h"
#include "ErrorInstance.h"

#include "CodeBlock.h"
#include "InlineCallFrame.h"
#include "Interpreter.h"
#include "JSScope.h"
#include "JSCInlines.h"
#include "ParseInt.h"
#include "StackFrame.h"
#include <wtf/text/StringBuilder.h>

namespace JSC {

const ClassInfo ErrorInstance::s_info = { "Error", &JSNonFinalObject::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(ErrorInstance) };

ErrorInstance::ErrorInstance(VM& vm, Structure* structure)
    : Base(vm, structure)
{
}

ErrorInstance* ErrorInstance::create(JSGlobalObject* globalObject, Structure* structure, JSValue message, SourceAppender appender, RuntimeType type, bool useCurrentFrame)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    String messageString = message.isUndefined() ? String() : message.toWTFString(globalObject);
    RETURN_IF_EXCEPTION(scope, nullptr);
    return create(globalObject, vm, structure, messageString, appender, type, useCurrentFrame);
}

static void appendSourceToError(JSGlobalObject* globalObject, CallFrame* callFrame, ErrorInstance* exception, BytecodeIndex bytecodeIndex)
{
    ErrorInstance::SourceAppender appender = exception->sourceAppender();
    exception->clearSourceAppender();
    RuntimeType type = exception->runtimeTypeForCause();
    exception->clearRuntimeTypeForCause();

    if (!callFrame->codeBlock()->hasExpressionInfo())
        return;
    
    int startOffset = 0;
    int endOffset = 0;
    int divotPoint = 0;
    unsigned line = 0;
    unsigned column = 0;

    CodeBlock* codeBlock;
    CodeOrigin codeOrigin = callFrame->codeOrigin();
    if (codeOrigin && codeOrigin.inlineCallFrame())
        codeBlock = baselineCodeBlockForInlineCallFrame(codeOrigin.inlineCallFrame());
    else
        codeBlock = callFrame->codeBlock();

    codeBlock->expressionRangeForBytecodeIndex(bytecodeIndex, divotPoint, startOffset, endOffset, line, column);
    
    int expressionStart = divotPoint - startOffset;
    int expressionStop = divotPoint + endOffset;

    StringView sourceString = codeBlock->source().provider()->source();
    if (!expressionStop || expressionStart > static_cast<int>(sourceString.length()))
        return;
    
    VM& vm = globalObject->vm();
    JSValue jsMessage = exception->getDirect(vm, vm.propertyNames->message);
    if (!jsMessage || !jsMessage.isString())
        return;
    
    String message = asString(jsMessage)->value(globalObject);
    if (expressionStart < expressionStop)
        message = appender(message, codeBlock->source().provider()->getRange(expressionStart, expressionStop).toString(), type, ErrorInstance::FoundExactSource);
    else {
        // No range information, so give a few characters of context.
        int dataLength = sourceString.length();
        int start = expressionStart;
        int stop = expressionStart;
        // Get up to 20 characters of context to the left and right of the divot, clamping to the line.
        // Then strip whitespace.
        while (start > 0 && (expressionStart - start < 20) && sourceString[start - 1] != '\n')
            start--;
        while (start < (expressionStart - 1) && isStrWhiteSpace(sourceString[start]))
            start++;
        while (stop < dataLength && (stop - expressionStart < 20) && sourceString[stop] != '\n')
            stop++;
        while (stop > expressionStart && isStrWhiteSpace(sourceString[stop - 1]))
            stop--;
        message = appender(message, codeBlock->source().provider()->getRange(start, stop).toString(), type, ErrorInstance::FoundApproximateSource);
    }
    exception->putDirect(vm, vm.propertyNames->message, jsString(vm, message));

}

void ErrorInstance::finishCreation(JSGlobalObject* globalObject, VM& vm, const String& message, bool useCurrentFrame)
{
    Base::finishCreation(vm);
    ASSERT(inherits(vm, info()));
    if (!message.isNull())
        putDirect(vm, vm.propertyNames->message, jsString(vm, message), static_cast<unsigned>(PropertyAttribute::DontEnum));

    std::unique_ptr<Vector<StackFrame>> stackTrace = getStackTrace(globalObject, vm, this, useCurrentFrame);
    {
        auto locker = holdLock(cellLock());
        m_stackTrace = WTFMove(stackTrace);
    }
    vm.heap.writeBarrier(this);

    if (m_stackTrace && !m_stackTrace->isEmpty() && hasSourceAppender()) {
        BytecodeIndex bytecodeIndex;
        CallFrame* callFrame;
        getBytecodeIndex(vm, vm.topCallFrame, m_stackTrace.get(), callFrame, bytecodeIndex);
        if (callFrame && callFrame->codeBlock() && !callFrame->callee().isWasm())
            appendSourceToError(globalObject, callFrame, this, bytecodeIndex);
    }
}

void ErrorInstance::destroy(JSCell* cell)
{
    static_cast<ErrorInstance*>(cell)->ErrorInstance::~ErrorInstance();
}

// Based on ErrorPrototype's errorProtoFuncToString(), but is modified to
// have no observable side effects to the user (i.e. does not call proxies,
// and getters).
String ErrorInstance::sanitizedToString(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    JSValue nameValue;
    auto namePropertName = vm.propertyNames->name;
    PropertySlot nameSlot(this, PropertySlot::InternalMethodType::VMInquiry);

    JSValue currentObj = this;
    unsigned prototypeDepth = 0;

    // We only check the current object and its prototype (2 levels) because normal
    // Error objects may have a name property, and if not, its prototype should have
    // a name property for the type of error e.g. "SyntaxError".
    while (currentObj.isCell() && prototypeDepth++ < 2) {
        JSObject* obj = jsCast<JSObject*>(currentObj);
        if (JSObject::getOwnPropertySlot(obj, globalObject, namePropertName, nameSlot) && nameSlot.isValue()) {
            nameValue = nameSlot.getValue(globalObject, namePropertName);
            break;
        }
        currentObj = obj->getPrototypeDirect(vm);
    }
    scope.assertNoException();

    String nameString;
    if (!nameValue)
        nameString = "Error"_s;
    else {
        nameString = nameValue.toWTFString(globalObject);
        RETURN_IF_EXCEPTION(scope, String());
    }

    JSValue messageValue;
    auto messagePropertName = vm.propertyNames->message;
    PropertySlot messageSlot(this, PropertySlot::InternalMethodType::VMInquiry);
    if (JSObject::getOwnPropertySlot(this, globalObject, messagePropertName, messageSlot) && messageSlot.isValue())
        messageValue = messageSlot.getValue(globalObject, messagePropertName);
    scope.assertNoException();

    String messageString;
    if (!messageValue)
        messageString = String();
    else {
        messageString = messageValue.toWTFString(globalObject);
        RETURN_IF_EXCEPTION(scope, String());
    }

    if (!nameString.length())
        return messageString;

    if (!messageString.length())
        return nameString;

    StringBuilder builder;
    builder.append(nameString);
    builder.appendLiteral(": ");
    builder.append(messageString);
    return builder.toString();
}

void ErrorInstance::finalizeUnconditionally(VM& vm)
{
    if (!m_stackTrace)
        return;

    // We don't want to keep our stack traces alive forever if the user doesn't access the stack trace.
    // If we did, we might end up keeping functions (and their global objects) alive that happened to
    // get caught in a trace.
    for (const auto& frame : *m_stackTrace.get()) {
        if (!frame.isMarked(vm)) {
            computeErrorInfo(vm);
            return;
        }
    }
}

void ErrorInstance::computeErrorInfo(VM& vm)
{
    ASSERT(!m_errorInfoMaterialized);

    if (m_stackTrace && !m_stackTrace->isEmpty()) {
        getLineColumnAndSource(m_stackTrace.get(), m_line, m_column, m_sourceURL);
        m_stackString = Interpreter::stackTraceAsString(vm, *m_stackTrace.get());
        m_stackTrace = nullptr;
    }
}

bool ErrorInstance::materializeErrorInfoIfNeeded(VM& vm)
{
    if (m_errorInfoMaterialized)
        return false;

    computeErrorInfo(vm);

    if (!m_stackString.isNull()) {
        auto attributes = static_cast<unsigned>(PropertyAttribute::DontEnum);

        putDirect(vm, vm.propertyNames->line, jsNumber(m_line), attributes);
        putDirect(vm, vm.propertyNames->column, jsNumber(m_column), attributes);
        if (!m_sourceURL.isEmpty())
            putDirect(vm, vm.propertyNames->sourceURL, jsString(vm, WTFMove(m_sourceURL)), attributes);

        putDirect(vm, vm.propertyNames->stack, jsString(vm, WTFMove(m_stackString)), attributes);
    }

    m_errorInfoMaterialized = true;
    return true;
}

bool ErrorInstance::materializeErrorInfoIfNeeded(VM& vm, PropertyName propertyName)
{
    if (propertyName == vm.propertyNames->line
        || propertyName == vm.propertyNames->column
        || propertyName == vm.propertyNames->sourceURL
        || propertyName == vm.propertyNames->stack)
        return materializeErrorInfoIfNeeded(vm);
    return false;
}

bool ErrorInstance::getOwnPropertySlot(JSObject* object, JSGlobalObject* globalObject, PropertyName propertyName, PropertySlot& slot)
{
    VM& vm = globalObject->vm();
    ErrorInstance* thisObject = jsCast<ErrorInstance*>(object);
    thisObject->materializeErrorInfoIfNeeded(vm, propertyName);
    return Base::getOwnPropertySlot(thisObject, globalObject, propertyName, slot);
}

void ErrorInstance::getOwnNonIndexPropertyNames(JSObject* object, JSGlobalObject* globalObject, PropertyNameArray& propertyNameArray, EnumerationMode enumerationMode)
{
    VM& vm = globalObject->vm();
    ErrorInstance* thisObject = jsCast<ErrorInstance*>(object);
    thisObject->materializeErrorInfoIfNeeded(vm);
    Base::getOwnNonIndexPropertyNames(thisObject, globalObject, propertyNameArray, enumerationMode);
}

void ErrorInstance::getStructurePropertyNames(JSObject* object, JSGlobalObject* globalObject, PropertyNameArray& propertyNameArray, EnumerationMode enumerationMode)
{
    VM& vm = globalObject->vm();
    ErrorInstance* thisObject = jsCast<ErrorInstance*>(object);
    thisObject->materializeErrorInfoIfNeeded(vm);
    Base::getStructurePropertyNames(thisObject, globalObject, propertyNameArray, enumerationMode);
}

bool ErrorInstance::defineOwnProperty(JSObject* object, JSGlobalObject* globalObject, PropertyName propertyName, const PropertyDescriptor& descriptor, bool shouldThrow)
{
    VM& vm = globalObject->vm();
    ErrorInstance* thisObject = jsCast<ErrorInstance*>(object);
    thisObject->materializeErrorInfoIfNeeded(vm, propertyName);
    return Base::defineOwnProperty(thisObject, globalObject, propertyName, descriptor, shouldThrow);
}

bool ErrorInstance::put(JSCell* cell, JSGlobalObject* globalObject, PropertyName propertyName, JSValue value, PutPropertySlot& slot)
{
    VM& vm = globalObject->vm();
    ErrorInstance* thisObject = jsCast<ErrorInstance*>(cell);
    bool materializedProperties = thisObject->materializeErrorInfoIfNeeded(vm, propertyName);
    if (materializedProperties)
        slot.disableCaching();
    return Base::put(thisObject, globalObject, propertyName, value, slot);
}

bool ErrorInstance::deleteProperty(JSCell* cell, JSGlobalObject* globalObject, PropertyName propertyName)
{
    VM& vm = globalObject->vm();
    ErrorInstance* thisObject = jsCast<ErrorInstance*>(cell);
    thisObject->materializeErrorInfoIfNeeded(vm, propertyName);
    return Base::deleteProperty(thisObject, globalObject, propertyName);
}

} // namespace JSC
