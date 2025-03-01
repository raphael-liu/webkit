/*
 * Copyright (C) 2013-2019 Apple Inc. All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE. 
 */

#include "config.h"
#include "JITOperations.h"

#if ENABLE(JIT)

#include "ArithProfile.h"
#include "ArrayConstructor.h"
#include "CommonSlowPaths.h"
#include "DFGCompilationMode.h"
#include "DFGDriver.h"
#include "DFGOSREntry.h"
#include "DFGThunks.h"
#include "DFGWorklist.h"
#include "Debugger.h"
#include "DirectArguments.h"
#include "Error.h"
#include "ErrorHandlingScope.h"
#include "EvalCodeBlock.h"
#include "ExceptionFuzz.h"
#include "ExecutableBaseInlines.h"
#include "FTLOSREntry.h"
#include "FrameTracers.h"
#include "FunctionCodeBlock.h"
#include "GetterSetter.h"
#include "HostCallReturnValue.h"
#include "ICStats.h"
#include "Interpreter.h"
#include "JIT.h"
#include "JITExceptions.h"
#include "JITToDFGDeferredCompilationCallback.h"
#include "JSAsyncFunction.h"
#include "JSAsyncGenerator.h"
#include "JSAsyncGeneratorFunction.h"
#include "JSCInlines.h"
#include "JSCPtrTag.h"
#include "JSGeneratorFunction.h"
#include "JSGlobalObjectFunctions.h"
#include "JSInternalPromise.h"
#include "JSLexicalEnvironment.h"
#include "JSWithScope.h"
#include "ModuleProgramCodeBlock.h"
#include "ObjectConstructor.h"
#include "PolymorphicAccess.h"
#include "ProgramCodeBlock.h"
#include "PropertyName.h"
#include "RegExpObject.h"
#include "Repatch.h"
#include "ScopedArguments.h"
#include "ShadowChicken.h"
#include "StructureStubInfo.h"
#include "SuperSampler.h"
#include "TestRunnerUtils.h"
#include "ThunkGenerators.h"
#include "TypeProfilerLog.h"
#include "VMInlines.h"
#include "WebAssemblyFunction.h"
#include <wtf/InlineASM.h>

IGNORE_WARNINGS_BEGIN("frame-address")

namespace JSC {

ALWAYS_INLINE JSValue profiledAdd(JSGlobalObject* globalObject, JSValue op1, JSValue op2, BinaryArithProfile& arithProfile)
{
    arithProfile.observeLHSAndRHS(op1, op2);
    JSValue result = jsAdd(globalObject, op1, op2);
    arithProfile.observeResult(result);
    return result;
}

extern "C" {

#if COMPILER(MSVC)
void * _ReturnAddress(void);
#pragma intrinsic(_ReturnAddress)

#define OUR_RETURN_ADDRESS _ReturnAddress()
#else
#define OUR_RETURN_ADDRESS __builtin_return_address(0)
#endif

#if ENABLE(OPCODE_SAMPLING)
#define CTI_SAMPLER vm.interpreter->sampler()
#else
#define CTI_SAMPLER 0
#endif


void JIT_OPERATION operationThrowStackOverflowError(CodeBlock* codeBlock)
{
    // We pass in our own code block, because the callframe hasn't been populated.
    VM& vm = codeBlock->vm();
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);
    auto scope = DECLARE_THROW_SCOPE(vm);
    callFrame->convertToStackOverflowFrame(vm, codeBlock);
    throwStackOverflowError(codeBlock->globalObject(), scope);
}

void JIT_OPERATION operationThrowStackOverflowErrorFromThunk(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);
    auto scope = DECLARE_THROW_SCOPE(vm);
    throwStackOverflowError(globalObject, scope);
    genericUnwind(vm, callFrame);
    ASSERT(vm.targetMachinePCForThrow);
}

int32_t JIT_OPERATION operationCallArityCheck(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);
    auto scope = DECLARE_THROW_SCOPE(vm);

    int32_t missingArgCount = CommonSlowPaths::arityCheckFor(vm, callFrame, CodeForCall);
    if (UNLIKELY(missingArgCount < 0)) {
        CodeBlock* codeBlock = CommonSlowPaths::codeBlockFromCallFrameCallee(callFrame, CodeForCall);
        callFrame->convertToStackOverflowFrame(vm, codeBlock);
        throwStackOverflowError(globalObject, scope);
    }

    return missingArgCount;
}

int32_t JIT_OPERATION operationConstructArityCheck(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);
    auto scope = DECLARE_THROW_SCOPE(vm);

    int32_t missingArgCount = CommonSlowPaths::arityCheckFor(vm, callFrame, CodeForConstruct);
    if (UNLIKELY(missingArgCount < 0)) {
        CodeBlock* codeBlock = CommonSlowPaths::codeBlockFromCallFrameCallee(callFrame, CodeForConstruct);
        callFrame->convertToStackOverflowFrame(vm, codeBlock);
        throwStackOverflowError(globalObject, scope);
    }

    return missingArgCount;
}

EncodedJSValue JIT_OPERATION operationTryGetById(JSGlobalObject* globalObject, StructureStubInfo* stubInfo, EncodedJSValue base, UniquedStringImpl* uid)
{
    VM& vm = globalObject->vm();
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);
    Identifier ident = Identifier::fromUid(vm, uid);
    stubInfo->tookSlowPath = true;

    JSValue baseValue = JSValue::decode(base);
    PropertySlot slot(baseValue, PropertySlot::InternalMethodType::VMInquiry);
    baseValue.getPropertySlot(globalObject, ident, slot);

    return JSValue::encode(slot.getPureResult());
}


EncodedJSValue JIT_OPERATION operationTryGetByIdGeneric(JSGlobalObject* globalObject, EncodedJSValue base, UniquedStringImpl* uid)
{
    VM& vm = globalObject->vm();
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);
    Identifier ident = Identifier::fromUid(vm, uid);

    JSValue baseValue = JSValue::decode(base);
    PropertySlot slot(baseValue, PropertySlot::InternalMethodType::VMInquiry);
    baseValue.getPropertySlot(globalObject, ident, slot);

    return JSValue::encode(slot.getPureResult());
}

EncodedJSValue JIT_OPERATION operationTryGetByIdOptimize(JSGlobalObject* globalObject, StructureStubInfo* stubInfo, EncodedJSValue base, UniquedStringImpl* uid)
{
    VM& vm = globalObject->vm();
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);
    auto scope = DECLARE_THROW_SCOPE(vm);
    Identifier ident = Identifier::fromUid(vm, uid);

    JSValue baseValue = JSValue::decode(base);
    PropertySlot slot(baseValue, PropertySlot::InternalMethodType::VMInquiry);

    baseValue.getPropertySlot(globalObject, ident, slot);
    RETURN_IF_EXCEPTION(scope, encodedJSValue());

    CodeBlock* codeBlock = callFrame->codeBlock();
    if (stubInfo->considerCaching(vm, codeBlock, baseValue.structureOrNull()) && !slot.isTaintedByOpaqueObject() && (slot.isCacheableValue() || slot.isCacheableGetter() || slot.isUnset()))
        repatchGetBy(globalObject, codeBlock, baseValue, ident, slot, *stubInfo, GetByKind::Try);

    return JSValue::encode(slot.getPureResult());
}

EncodedJSValue JIT_OPERATION operationGetByIdDirect(JSGlobalObject* globalObject, StructureStubInfo* stubInfo, EncodedJSValue base, UniquedStringImpl* uid)
{
    VM& vm = globalObject->vm();
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);
    auto scope = DECLARE_THROW_SCOPE(vm);
    Identifier ident = Identifier::fromUid(vm, uid);
    stubInfo->tookSlowPath = true;

    JSValue baseValue = JSValue::decode(base);
    PropertySlot slot(baseValue, PropertySlot::InternalMethodType::GetOwnProperty);

    bool found = baseValue.getOwnPropertySlot(globalObject, ident, slot);
    RETURN_IF_EXCEPTION(scope, encodedJSValue());

    RELEASE_AND_RETURN(scope, JSValue::encode(found ? slot.getValue(globalObject, ident) : jsUndefined()));
}

EncodedJSValue JIT_OPERATION operationGetByIdDirectGeneric(JSGlobalObject* globalObject, EncodedJSValue base, UniquedStringImpl* uid)
{
    VM& vm = globalObject->vm();
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);
    auto scope = DECLARE_THROW_SCOPE(vm);
    Identifier ident = Identifier::fromUid(vm, uid);

    JSValue baseValue = JSValue::decode(base);
    PropertySlot slot(baseValue, PropertySlot::InternalMethodType::GetOwnProperty);

    bool found = baseValue.getOwnPropertySlot(globalObject, ident, slot);
    RETURN_IF_EXCEPTION(scope, encodedJSValue());

    RELEASE_AND_RETURN(scope, JSValue::encode(found ? slot.getValue(globalObject, ident) : jsUndefined()));
}

EncodedJSValue JIT_OPERATION operationGetByIdDirectOptimize(JSGlobalObject* globalObject, StructureStubInfo* stubInfo, EncodedJSValue base, UniquedStringImpl* uid)
{
    VM& vm = globalObject->vm();
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);
    auto scope = DECLARE_THROW_SCOPE(vm);
    Identifier ident = Identifier::fromUid(vm, uid);

    JSValue baseValue = JSValue::decode(base);
    PropertySlot slot(baseValue, PropertySlot::InternalMethodType::GetOwnProperty);

    bool found = baseValue.getOwnPropertySlot(globalObject, ident, slot);
    RETURN_IF_EXCEPTION(scope, encodedJSValue());

    CodeBlock* codeBlock = callFrame->codeBlock();
    if (stubInfo->considerCaching(vm, codeBlock, baseValue.structureOrNull()))
        repatchGetBy(globalObject, codeBlock, baseValue, ident, slot, *stubInfo, GetByKind::Direct);

    RELEASE_AND_RETURN(scope, JSValue::encode(found ? slot.getValue(globalObject, ident) : jsUndefined()));
}

EncodedJSValue JIT_OPERATION operationGetById(JSGlobalObject* globalObject, StructureStubInfo* stubInfo, EncodedJSValue base, UniquedStringImpl* uid)
{
    SuperSamplerScope superSamplerScope(false);
    
    VM& vm = globalObject->vm();
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);
    
    stubInfo->tookSlowPath = true;
    
    JSValue baseValue = JSValue::decode(base);
    PropertySlot slot(baseValue, PropertySlot::InternalMethodType::Get);
    Identifier ident = Identifier::fromUid(vm, uid);
    JSValue result = baseValue.get(globalObject, ident, slot);

    LOG_IC((ICEvent::OperationGetById, baseValue.classInfoOrNull(vm), ident, baseValue == slot.slotBase()));

    return JSValue::encode(result);
}

EncodedJSValue JIT_OPERATION operationGetByIdGeneric(JSGlobalObject* globalObject, EncodedJSValue base, UniquedStringImpl* uid)
{
    SuperSamplerScope superSamplerScope(false);
    
    VM& vm = globalObject->vm();
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);
    
    JSValue baseValue = JSValue::decode(base);
    PropertySlot slot(baseValue, PropertySlot::InternalMethodType::Get);
    Identifier ident = Identifier::fromUid(vm, uid);
    JSValue result = baseValue.get(globalObject, ident, slot);
    
    LOG_IC((ICEvent::OperationGetByIdGeneric, baseValue.classInfoOrNull(vm), ident, baseValue == slot.slotBase()));
    
    return JSValue::encode(result);
}

EncodedJSValue JIT_OPERATION operationGetByIdOptimize(JSGlobalObject* globalObject, StructureStubInfo* stubInfo, EncodedJSValue base, UniquedStringImpl* uid)
{
    SuperSamplerScope superSamplerScope(false);
    
    VM& vm = globalObject->vm();
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);
    Identifier ident = Identifier::fromUid(vm, uid);

    JSValue baseValue = JSValue::decode(base);

    return JSValue::encode(baseValue.getPropertySlot(globalObject, ident, [&] (bool found, PropertySlot& slot) -> JSValue {
        
        LOG_IC((ICEvent::OperationGetByIdOptimize, baseValue.classInfoOrNull(vm), ident, baseValue == slot.slotBase()));
        
        CodeBlock* codeBlock = callFrame->codeBlock();
        if (stubInfo->considerCaching(vm, codeBlock, baseValue.structureOrNull()))
            repatchGetBy(globalObject, codeBlock, baseValue, ident, slot, *stubInfo, GetByKind::Normal);
        return found ? slot.getValue(globalObject, ident) : jsUndefined();
    }));
}

EncodedJSValue JIT_OPERATION operationGetByIdWithThis(JSGlobalObject* globalObject, StructureStubInfo* stubInfo, EncodedJSValue base, EncodedJSValue thisEncoded, UniquedStringImpl* uid)
{
    SuperSamplerScope superSamplerScope(false);

    VM& vm = globalObject->vm();
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);
    Identifier ident = Identifier::fromUid(vm, uid);

    stubInfo->tookSlowPath = true;

    JSValue baseValue = JSValue::decode(base);
    JSValue thisValue = JSValue::decode(thisEncoded);
    PropertySlot slot(thisValue, PropertySlot::InternalMethodType::Get);

    return JSValue::encode(baseValue.get(globalObject, ident, slot));
}

EncodedJSValue JIT_OPERATION operationGetByIdWithThisGeneric(JSGlobalObject* globalObject, EncodedJSValue base, EncodedJSValue thisEncoded, UniquedStringImpl* uid)
{
    SuperSamplerScope superSamplerScope(false);

    VM& vm = globalObject->vm();
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);
    Identifier ident = Identifier::fromUid(vm, uid);

    JSValue baseValue = JSValue::decode(base);
    JSValue thisValue = JSValue::decode(thisEncoded);
    PropertySlot slot(thisValue, PropertySlot::InternalMethodType::Get);

    return JSValue::encode(baseValue.get(globalObject, ident, slot));
}

EncodedJSValue JIT_OPERATION operationGetByIdWithThisOptimize(JSGlobalObject* globalObject, StructureStubInfo* stubInfo, EncodedJSValue base, EncodedJSValue thisEncoded, UniquedStringImpl* uid)
{
    SuperSamplerScope superSamplerScope(false);
    
    VM& vm = globalObject->vm();
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);
    Identifier ident = Identifier::fromUid(vm, uid);

    JSValue baseValue = JSValue::decode(base);
    JSValue thisValue = JSValue::decode(thisEncoded);

    PropertySlot slot(thisValue, PropertySlot::InternalMethodType::Get);
    return JSValue::encode(baseValue.getPropertySlot(globalObject, ident, slot, [&] (bool found, PropertySlot& slot) -> JSValue {
        LOG_IC((ICEvent::OperationGetByIdWithThisOptimize, baseValue.classInfoOrNull(vm), ident, baseValue == slot.slotBase()));
        
        CodeBlock* codeBlock = callFrame->codeBlock();
        if (stubInfo->considerCaching(vm, codeBlock, baseValue.structureOrNull()))
            repatchGetBy(globalObject, codeBlock, baseValue, ident, slot, *stubInfo, GetByKind::WithThis);
        return found ? slot.getValue(globalObject, ident) : jsUndefined();
    }));
}

EncodedJSValue JIT_OPERATION operationInById(JSGlobalObject* globalObject, StructureStubInfo* stubInfo, EncodedJSValue base, UniquedStringImpl* uid)
{
    SuperSamplerScope superSamplerScope(false);

    VM& vm = globalObject->vm();
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);
    auto scope = DECLARE_THROW_SCOPE(vm);

    stubInfo->tookSlowPath = true;

    Identifier ident = Identifier::fromUid(vm, uid);

    JSValue baseValue = JSValue::decode(base);
    if (!baseValue.isObject()) {
        throwException(globalObject, scope, createInvalidInParameterError(globalObject, baseValue));
        return JSValue::encode(jsUndefined());
    }
    JSObject* baseObject = asObject(baseValue);

    LOG_IC((ICEvent::OperationInById, baseObject->classInfo(vm), ident));

    scope.release();
    PropertySlot slot(baseObject, PropertySlot::InternalMethodType::HasProperty);
    return JSValue::encode(jsBoolean(baseObject->getPropertySlot(globalObject, ident, slot)));
}

EncodedJSValue JIT_OPERATION operationInByIdGeneric(JSGlobalObject* globalObject, EncodedJSValue base, UniquedStringImpl* uid)
{
    SuperSamplerScope superSamplerScope(false);

    VM& vm = globalObject->vm();
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);
    auto scope = DECLARE_THROW_SCOPE(vm);

    Identifier ident = Identifier::fromUid(vm, uid);

    JSValue baseValue = JSValue::decode(base);
    if (!baseValue.isObject()) {
        throwException(globalObject, scope, createInvalidInParameterError(globalObject, baseValue));
        return JSValue::encode(jsUndefined());
    }
    JSObject* baseObject = asObject(baseValue);

    LOG_IC((ICEvent::OperationInByIdGeneric, baseObject->classInfo(vm), ident));

    scope.release();
    PropertySlot slot(baseObject, PropertySlot::InternalMethodType::HasProperty);
    return JSValue::encode(jsBoolean(baseObject->getPropertySlot(globalObject, ident, slot)));
}

EncodedJSValue JIT_OPERATION operationInByIdOptimize(JSGlobalObject* globalObject, StructureStubInfo* stubInfo, EncodedJSValue base, UniquedStringImpl* uid)
{
    SuperSamplerScope superSamplerScope(false);

    VM& vm = globalObject->vm();
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);
    auto scope = DECLARE_THROW_SCOPE(vm);

    Identifier ident = Identifier::fromUid(vm, uid);

    JSValue baseValue = JSValue::decode(base);
    if (!baseValue.isObject()) {
        throwException(globalObject, scope, createInvalidInParameterError(globalObject, baseValue));
        return JSValue::encode(jsUndefined());
    }
    JSObject* baseObject = asObject(baseValue);

    LOG_IC((ICEvent::OperationInByIdOptimize, baseObject->classInfo(vm), ident));

    scope.release();
    PropertySlot slot(baseObject, PropertySlot::InternalMethodType::HasProperty);
    bool found = baseObject->getPropertySlot(globalObject, ident, slot);
    CodeBlock* codeBlock = callFrame->codeBlock();
    if (stubInfo->considerCaching(vm, codeBlock, baseObject->structure(vm)))
        repatchInByID(globalObject, codeBlock, baseObject, ident, found, slot, *stubInfo);
    return JSValue::encode(jsBoolean(found));
}

EncodedJSValue JIT_OPERATION operationInByVal(JSGlobalObject* globalObject, JSCell* base, EncodedJSValue key)
{
    SuperSamplerScope superSamplerScope(false);
    
    VM& vm = globalObject->vm();
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);

    return JSValue::encode(jsBoolean(CommonSlowPaths::opInByVal(globalObject, base, JSValue::decode(key))));
}

void JIT_OPERATION operationPutByIdStrict(JSGlobalObject* globalObject, StructureStubInfo* stubInfo, EncodedJSValue encodedValue, EncodedJSValue encodedBase, UniquedStringImpl* uid)
{
    SuperSamplerScope superSamplerScope(false);
    
    VM& vm = globalObject->vm();
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);
    
    stubInfo->tookSlowPath = true;
    
    JSValue baseValue = JSValue::decode(encodedBase);
    Identifier ident = Identifier::fromUid(vm, uid);
    PutPropertySlot slot(baseValue, true, callFrame->codeBlock()->putByIdContext());
    baseValue.putInline(globalObject, ident, JSValue::decode(encodedValue), slot);
    
    LOG_IC((ICEvent::OperationPutByIdStrict, baseValue.classInfoOrNull(vm), ident, slot.base() == baseValue));
}

void JIT_OPERATION operationPutByIdNonStrict(JSGlobalObject* globalObject, StructureStubInfo* stubInfo, EncodedJSValue encodedValue, EncodedJSValue encodedBase, UniquedStringImpl* uid)
{
    SuperSamplerScope superSamplerScope(false);
    
    VM& vm = globalObject->vm();
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);
    
    stubInfo->tookSlowPath = true;
    
    JSValue baseValue = JSValue::decode(encodedBase);
    Identifier ident = Identifier::fromUid(vm, uid);
    PutPropertySlot slot(baseValue, false, callFrame->codeBlock()->putByIdContext());
    baseValue.putInline(globalObject, ident, JSValue::decode(encodedValue), slot);

    LOG_IC((ICEvent::OperationPutByIdNonStrict, baseValue.classInfoOrNull(vm), ident, slot.base() == baseValue));
}

void JIT_OPERATION operationPutByIdDirectStrict(JSGlobalObject* globalObject, StructureStubInfo* stubInfo, EncodedJSValue encodedValue, EncodedJSValue encodedBase, UniquedStringImpl* uid)
{
    SuperSamplerScope superSamplerScope(false);
    
    VM& vm = globalObject->vm();
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);
    
    stubInfo->tookSlowPath = true;
    
    JSValue baseValue = JSValue::decode(encodedBase);
    Identifier ident = Identifier::fromUid(vm, uid);
    PutPropertySlot slot(baseValue, true, callFrame->codeBlock()->putByIdContext());
    CommonSlowPaths::putDirectWithReify(vm, globalObject, asObject(baseValue), ident, JSValue::decode(encodedValue), slot);

    LOG_IC((ICEvent::OperationPutByIdDirectStrict, baseValue.classInfoOrNull(vm), ident, slot.base() == baseValue));
}

void JIT_OPERATION operationPutByIdDirectNonStrict(JSGlobalObject* globalObject, StructureStubInfo* stubInfo, EncodedJSValue encodedValue, EncodedJSValue encodedBase, UniquedStringImpl* uid)
{
    SuperSamplerScope superSamplerScope(false);
    
    VM& vm = globalObject->vm();
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);
    
    stubInfo->tookSlowPath = true;
    
    JSValue baseValue = JSValue::decode(encodedBase);
    Identifier ident = Identifier::fromUid(vm, uid);
    PutPropertySlot slot(baseValue, false, callFrame->codeBlock()->putByIdContext());
    CommonSlowPaths::putDirectWithReify(vm, globalObject, asObject(baseValue), ident, JSValue::decode(encodedValue), slot);

    LOG_IC((ICEvent::OperationPutByIdDirectNonStrict, baseValue.classInfoOrNull(vm), ident, slot.base() == baseValue));
}

void JIT_OPERATION operationPutByIdStrictOptimize(JSGlobalObject* globalObject, StructureStubInfo* stubInfo, EncodedJSValue encodedValue, EncodedJSValue encodedBase, UniquedStringImpl* uid)
{
    SuperSamplerScope superSamplerScope(false);
    
    VM& vm = globalObject->vm();
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);
    auto scope = DECLARE_THROW_SCOPE(vm);

    Identifier ident = Identifier::fromUid(vm, uid);
    AccessType accessType = static_cast<AccessType>(stubInfo->accessType);

    JSValue value = JSValue::decode(encodedValue);
    JSValue baseValue = JSValue::decode(encodedBase);
    CodeBlock* codeBlock = callFrame->codeBlock();
    PutPropertySlot slot(baseValue, true, codeBlock->putByIdContext());

    Structure* structure = baseValue.isCell() ? baseValue.asCell()->structure(vm) : nullptr;
    baseValue.putInline(globalObject, ident, value, slot);

    LOG_IC((ICEvent::OperationPutByIdStrictOptimize, baseValue.classInfoOrNull(vm), ident, slot.base() == baseValue));

    RETURN_IF_EXCEPTION(scope, void());

    if (accessType != static_cast<AccessType>(stubInfo->accessType))
        return;
    
    if (stubInfo->considerCaching(vm, codeBlock, structure))
        repatchPutByID(globalObject, codeBlock, baseValue, structure, ident, slot, *stubInfo, NotDirect);
}

void JIT_OPERATION operationPutByIdNonStrictOptimize(JSGlobalObject* globalObject, StructureStubInfo* stubInfo, EncodedJSValue encodedValue, EncodedJSValue encodedBase, UniquedStringImpl* uid)
{
    SuperSamplerScope superSamplerScope(false);
    
    VM& vm = globalObject->vm();
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);
    auto scope = DECLARE_THROW_SCOPE(vm);

    Identifier ident = Identifier::fromUid(vm, uid);
    AccessType accessType = static_cast<AccessType>(stubInfo->accessType);

    JSValue value = JSValue::decode(encodedValue);
    JSValue baseValue = JSValue::decode(encodedBase);
    CodeBlock* codeBlock = callFrame->codeBlock();
    PutPropertySlot slot(baseValue, false, codeBlock->putByIdContext());

    Structure* structure = baseValue.isCell() ? baseValue.asCell()->structure(vm) : nullptr;
    baseValue.putInline(globalObject, ident, value, slot);

    LOG_IC((ICEvent::OperationPutByIdNonStrictOptimize, baseValue.classInfoOrNull(vm), ident, slot.base() == baseValue));

    RETURN_IF_EXCEPTION(scope, void());

    if (accessType != static_cast<AccessType>(stubInfo->accessType))
        return;
    
    if (stubInfo->considerCaching(vm, codeBlock, structure))
        repatchPutByID(globalObject, codeBlock, baseValue, structure, ident, slot, *stubInfo, NotDirect);
}

void JIT_OPERATION operationPutByIdDirectStrictOptimize(JSGlobalObject* globalObject, StructureStubInfo* stubInfo, EncodedJSValue encodedValue, EncodedJSValue encodedBase, UniquedStringImpl* uid)
{
    SuperSamplerScope superSamplerScope(false);
    
    VM& vm = globalObject->vm();
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);
    auto scope = DECLARE_THROW_SCOPE(vm);
    
    Identifier ident = Identifier::fromUid(vm, uid);
    AccessType accessType = static_cast<AccessType>(stubInfo->accessType);

    JSValue value = JSValue::decode(encodedValue);
    JSObject* baseObject = asObject(JSValue::decode(encodedBase));
    CodeBlock* codeBlock = callFrame->codeBlock();
    PutPropertySlot slot(baseObject, true, codeBlock->putByIdContext());
    Structure* structure = nullptr;
    CommonSlowPaths::putDirectWithReify(vm, globalObject, baseObject, ident, value, slot, &structure);

    LOG_IC((ICEvent::OperationPutByIdDirectStrictOptimize, baseObject->classInfo(vm), ident, slot.base() == baseObject));

    RETURN_IF_EXCEPTION(scope, void());
    
    if (accessType != static_cast<AccessType>(stubInfo->accessType))
        return;
    
    if (stubInfo->considerCaching(vm, codeBlock, structure))
        repatchPutByID(globalObject, codeBlock, baseObject, structure, ident, slot, *stubInfo, Direct);
}

void JIT_OPERATION operationPutByIdDirectNonStrictOptimize(JSGlobalObject* globalObject, StructureStubInfo* stubInfo, EncodedJSValue encodedValue, EncodedJSValue encodedBase, UniquedStringImpl* uid)
{
    SuperSamplerScope superSamplerScope(false);
    
    VM& vm = globalObject->vm();
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);
    auto scope = DECLARE_THROW_SCOPE(vm);
    
    Identifier ident = Identifier::fromUid(vm, uid);
    AccessType accessType = static_cast<AccessType>(stubInfo->accessType);

    JSValue value = JSValue::decode(encodedValue);
    JSObject* baseObject = asObject(JSValue::decode(encodedBase));
    CodeBlock* codeBlock = callFrame->codeBlock();
    PutPropertySlot slot(baseObject, false, codeBlock->putByIdContext());
    Structure* structure = nullptr;
    CommonSlowPaths::putDirectWithReify(vm, globalObject, baseObject, ident, value, slot, &structure);

    LOG_IC((ICEvent::OperationPutByIdDirectNonStrictOptimize, baseObject->classInfo(vm), ident, slot.base() == baseObject));

    RETURN_IF_EXCEPTION(scope, void());
    
    if (accessType != static_cast<AccessType>(stubInfo->accessType))
        return;
    
    if (stubInfo->considerCaching(vm, codeBlock, structure))
        repatchPutByID(globalObject, codeBlock, baseObject, structure, ident, slot, *stubInfo, Direct);
}

ALWAYS_INLINE static bool isStringOrSymbol(JSValue value)
{
    return value.isString() || value.isSymbol();
}

static void putByVal(JSGlobalObject* globalObject, CodeBlock* codeBlock, JSValue baseValue, JSValue subscript, JSValue value, ByValInfo* byValInfo)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (LIKELY(subscript.isUInt32())) {
        byValInfo->tookSlowPath = true;
        uint32_t i = subscript.asUInt32();
        if (baseValue.isObject()) {
            JSObject* object = asObject(baseValue);
            if (object->canSetIndexQuickly(i, value)) {
                object->setIndexQuickly(vm, i, value);
                return;
            }

            byValInfo->arrayProfile->setOutOfBounds();
            scope.release();
            object->methodTable(vm)->putByIndex(object, globalObject, i, value, codeBlock->isStrictMode());
            return;
        }

        scope.release();
        baseValue.putByIndex(globalObject, i, value, codeBlock->isStrictMode());
        return;
    } else if (subscript.isInt32()) {
        byValInfo->tookSlowPath = true;
        if (baseValue.isObject())
            byValInfo->arrayProfile->setOutOfBounds();
    }

    auto property = subscript.toPropertyKey(globalObject);
    // Don't put to an object if toString threw an exception.
    RETURN_IF_EXCEPTION(scope, void());

    if (byValInfo->stubInfo && (!isStringOrSymbol(subscript) || byValInfo->cachedId != property))
        byValInfo->tookSlowPath = true;

    scope.release();
    PutPropertySlot slot(baseValue, codeBlock->isStrictMode());
    baseValue.putInline(globalObject, property, value, slot);
}

static void directPutByVal(JSGlobalObject* globalObject, CodeBlock* codeBlock, JSObject* baseObject, JSValue subscript, JSValue value, ByValInfo* byValInfo)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    bool isStrictMode = codeBlock->isStrictMode();

    if (LIKELY(subscript.isUInt32())) {
        // Despite its name, JSValue::isUInt32 will return true only for positive boxed int32_t; all those values are valid array indices.
        byValInfo->tookSlowPath = true;
        uint32_t index = subscript.asUInt32();
        ASSERT(isIndex(index));

        switch (baseObject->indexingType()) {
        case ALL_INT32_INDEXING_TYPES:
        case ALL_DOUBLE_INDEXING_TYPES:
        case ALL_CONTIGUOUS_INDEXING_TYPES:
        case ALL_ARRAY_STORAGE_INDEXING_TYPES:
            if (index < baseObject->butterfly()->vectorLength())
                break;
            FALLTHROUGH;
        default:
            byValInfo->arrayProfile->setOutOfBounds();
            break;
        }

        scope.release();
        baseObject->putDirectIndex(globalObject, index, value, 0, isStrictMode ? PutDirectIndexShouldThrow : PutDirectIndexShouldNotThrow);
        return;
    }

    if (subscript.isDouble()) {
        double subscriptAsDouble = subscript.asDouble();
        uint32_t subscriptAsUInt32 = static_cast<uint32_t>(subscriptAsDouble);
        if (subscriptAsDouble == subscriptAsUInt32 && isIndex(subscriptAsUInt32)) {
            byValInfo->tookSlowPath = true;
            scope.release();
            baseObject->putDirectIndex(globalObject, subscriptAsUInt32, value, 0, isStrictMode ? PutDirectIndexShouldThrow : PutDirectIndexShouldNotThrow);
            return;
        }
    }

    // Don't put to an object if toString threw an exception.
    auto property = subscript.toPropertyKey(globalObject);
    RETURN_IF_EXCEPTION(scope, void());

    if (Optional<uint32_t> index = parseIndex(property)) {
        byValInfo->tookSlowPath = true;
        scope.release();
        baseObject->putDirectIndex(globalObject, index.value(), value, 0, isStrictMode ? PutDirectIndexShouldThrow : PutDirectIndexShouldNotThrow);
        return;
    }

    if (byValInfo->stubInfo && (!isStringOrSymbol(subscript) || byValInfo->cachedId != property))
        byValInfo->tookSlowPath = true;

    scope.release();
    PutPropertySlot slot(baseObject, isStrictMode);
    CommonSlowPaths::putDirectWithReify(vm, globalObject, baseObject, property, value, slot);
}

enum class OptimizationResult {
    NotOptimized,
    SeenOnce,
    Optimized,
    GiveUp,
};

static OptimizationResult tryPutByValOptimize(JSGlobalObject* globalObject, CallFrame* callFrame, CodeBlock* codeBlock, JSValue baseValue, JSValue subscript, ByValInfo* byValInfo, ReturnAddressPtr returnAddress)
{
    UNUSED_PARAM(callFrame);

    // See if it's worth optimizing at all.
    OptimizationResult optimizationResult = OptimizationResult::NotOptimized;

    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    if (baseValue.isObject() && isCopyOnWrite(baseValue.getObject()->indexingMode()))
        return OptimizationResult::GiveUp;

    if (baseValue.isObject() && subscript.isInt32()) {
        JSObject* object = asObject(baseValue);

        ASSERT(callFrame->bytecodeIndex() != BytecodeIndex(0));
        ASSERT(!byValInfo->stubRoutine);

        Structure* structure = object->structure(vm);
        if (hasOptimizableIndexing(structure)) {
            // Attempt to optimize.
            JITArrayMode arrayMode = jitArrayModeForStructure(structure);
            if (jitArrayModePermitsPut(arrayMode) && arrayMode != byValInfo->arrayMode) {
                ConcurrentJSLocker locker(codeBlock->m_lock);
                byValInfo->arrayProfile->computeUpdatedPrediction(locker, codeBlock, structure);
                JIT::compilePutByVal(locker, vm, codeBlock, byValInfo, returnAddress, arrayMode);
                optimizationResult = OptimizationResult::Optimized;
            }
        }

        // If we failed to patch and we have some object that intercepts indexed get, then don't even wait until 10 times.
        if (optimizationResult != OptimizationResult::Optimized && object->structure(vm)->typeInfo().interceptsGetOwnPropertySlotByIndexEvenWhenLengthIsNotZero())
            optimizationResult = OptimizationResult::GiveUp;
    }

    if (baseValue.isObject() && isStringOrSymbol(subscript)) {
        const Identifier propertyName = subscript.toPropertyKey(globalObject);
        RETURN_IF_EXCEPTION(scope, OptimizationResult::GiveUp);
        if (subscript.isSymbol() || !parseIndex(propertyName)) {
            ASSERT(callFrame->bytecodeIndex() != BytecodeIndex(0));
            ASSERT(!byValInfo->stubRoutine);
            if (byValInfo->seen) {
                if (byValInfo->cachedId == propertyName) {
                    JIT::compilePutByValWithCachedId<OpPutByVal>(vm, codeBlock, byValInfo, returnAddress, NotDirect, propertyName);
                    optimizationResult = OptimizationResult::Optimized;
                } else {
                    // Seem like a generic property access site.
                    optimizationResult = OptimizationResult::GiveUp;
                }
            } else {
                ConcurrentJSLocker locker(codeBlock->m_lock);
                byValInfo->seen = true;
                byValInfo->cachedId = propertyName;
                if (subscript.isSymbol())
                    byValInfo->cachedSymbol.set(vm, codeBlock, asSymbol(subscript));
                optimizationResult = OptimizationResult::SeenOnce;
            }
        }
    }

    if (optimizationResult != OptimizationResult::Optimized && optimizationResult != OptimizationResult::SeenOnce) {
        // If we take slow path more than 10 times without patching then make sure we
        // never make that mistake again. For cases where we see non-index-intercepting
        // objects, this gives 10 iterations worth of opportunity for us to observe
        // that the put_by_val may be polymorphic. We count up slowPathCount even if
        // the result is GiveUp.
        if (++byValInfo->slowPathCount >= 10)
            optimizationResult = OptimizationResult::GiveUp;
    }

    return optimizationResult;
}

void JIT_OPERATION operationPutByValOptimize(JSGlobalObject* globalObject, EncodedJSValue encodedBaseValue, EncodedJSValue encodedSubscript, EncodedJSValue encodedValue, ByValInfo* byValInfo)
{
    VM& vm = globalObject->vm();
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);
    auto scope = DECLARE_THROW_SCOPE(vm);

    JSValue baseValue = JSValue::decode(encodedBaseValue);
    JSValue subscript = JSValue::decode(encodedSubscript);
    JSValue value = JSValue::decode(encodedValue);
    CodeBlock* codeBlock = callFrame->codeBlock();
    OptimizationResult result = tryPutByValOptimize(globalObject, callFrame, codeBlock, baseValue, subscript, byValInfo, ReturnAddressPtr(OUR_RETURN_ADDRESS));
    RETURN_IF_EXCEPTION(scope, void());
    if (result == OptimizationResult::GiveUp) {
        // Don't ever try to optimize.
        byValInfo->tookSlowPath = true;
        ctiPatchCallByReturnAddress(ReturnAddressPtr(OUR_RETURN_ADDRESS), operationPutByValGeneric);
    }
    RELEASE_AND_RETURN(scope, putByVal(globalObject, codeBlock, baseValue, subscript, value, byValInfo));
}

static OptimizationResult tryDirectPutByValOptimize(JSGlobalObject* globalObject, CallFrame* callFrame, CodeBlock* codeBlock, JSObject* object, JSValue subscript, ByValInfo* byValInfo, ReturnAddressPtr returnAddress)
{
    UNUSED_PARAM(callFrame);

    // See if it's worth optimizing at all.
    OptimizationResult optimizationResult = OptimizationResult::NotOptimized;

    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    if (subscript.isInt32()) {
        ASSERT(callFrame->bytecodeIndex() != BytecodeIndex(0));
        ASSERT(!byValInfo->stubRoutine);

        Structure* structure = object->structure(vm);
        if (hasOptimizableIndexing(structure)) {
            // Attempt to optimize.
            JITArrayMode arrayMode = jitArrayModeForStructure(structure);
            if (jitArrayModePermitsPutDirect(arrayMode) && arrayMode != byValInfo->arrayMode) {
                ConcurrentJSLocker locker(codeBlock->m_lock);
                byValInfo->arrayProfile->computeUpdatedPrediction(locker, codeBlock, structure);

                JIT::compileDirectPutByVal(locker, vm, codeBlock, byValInfo, returnAddress, arrayMode);
                optimizationResult = OptimizationResult::Optimized;
            }
        }

        // If we failed to patch and we have some object that intercepts indexed get, then don't even wait until 10 times.
        if (optimizationResult != OptimizationResult::Optimized && object->structure(vm)->typeInfo().interceptsGetOwnPropertySlotByIndexEvenWhenLengthIsNotZero())
            optimizationResult = OptimizationResult::GiveUp;
    } else if (isStringOrSymbol(subscript)) {
        const Identifier propertyName = subscript.toPropertyKey(globalObject);
        RETURN_IF_EXCEPTION(scope, OptimizationResult::GiveUp);
        if (subscript.isSymbol() || !parseIndex(propertyName)) {
            ASSERT(callFrame->bytecodeIndex() != BytecodeIndex(0));
            ASSERT(!byValInfo->stubRoutine);
            if (byValInfo->seen) {
                if (byValInfo->cachedId == propertyName) {
                    JIT::compilePutByValWithCachedId<OpPutByValDirect>(vm, codeBlock, byValInfo, returnAddress, Direct, propertyName);
                    optimizationResult = OptimizationResult::Optimized;
                } else {
                    // Seem like a generic property access site.
                    optimizationResult = OptimizationResult::GiveUp;
                }
            } else {
                ConcurrentJSLocker locker(codeBlock->m_lock);
                byValInfo->seen = true;
                byValInfo->cachedId = propertyName;
                if (subscript.isSymbol())
                    byValInfo->cachedSymbol.set(vm, codeBlock, asSymbol(subscript));
                optimizationResult = OptimizationResult::SeenOnce;
            }
        }
    }

    if (optimizationResult != OptimizationResult::Optimized && optimizationResult != OptimizationResult::SeenOnce) {
        // If we take slow path more than 10 times without patching then make sure we
        // never make that mistake again. For cases where we see non-index-intercepting
        // objects, this gives 10 iterations worth of opportunity for us to observe
        // that the get_by_val may be polymorphic. We count up slowPathCount even if
        // the result is GiveUp.
        if (++byValInfo->slowPathCount >= 10)
            optimizationResult = OptimizationResult::GiveUp;
    }

    return optimizationResult;
}

void JIT_OPERATION operationDirectPutByValOptimize(JSGlobalObject* globalObject, EncodedJSValue encodedBaseValue, EncodedJSValue encodedSubscript, EncodedJSValue encodedValue, ByValInfo* byValInfo)
{
    VM& vm = globalObject->vm();
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);
    auto scope = DECLARE_THROW_SCOPE(vm);

    JSValue baseValue = JSValue::decode(encodedBaseValue);
    JSValue subscript = JSValue::decode(encodedSubscript);
    JSValue value = JSValue::decode(encodedValue);
    RELEASE_ASSERT(baseValue.isObject());
    JSObject* object = asObject(baseValue);
    CodeBlock* codeBlock = callFrame->codeBlock();
    OptimizationResult result = tryDirectPutByValOptimize(globalObject, callFrame, codeBlock, object, subscript, byValInfo, ReturnAddressPtr(OUR_RETURN_ADDRESS));
    RETURN_IF_EXCEPTION(scope, void());
    if (result == OptimizationResult::GiveUp) {
        // Don't ever try to optimize.
        byValInfo->tookSlowPath = true;
        ctiPatchCallByReturnAddress(ReturnAddressPtr(OUR_RETURN_ADDRESS), operationDirectPutByValGeneric);
    }

    RELEASE_AND_RETURN(scope, directPutByVal(globalObject, codeBlock, object, subscript, value, byValInfo));
}

void JIT_OPERATION operationPutByValGeneric(JSGlobalObject* globalObject, EncodedJSValue encodedBaseValue, EncodedJSValue encodedSubscript, EncodedJSValue encodedValue, ByValInfo* byValInfo)
{
    VM& vm = globalObject->vm();
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);
    
    JSValue baseValue = JSValue::decode(encodedBaseValue);
    JSValue subscript = JSValue::decode(encodedSubscript);
    JSValue value = JSValue::decode(encodedValue);

    putByVal(globalObject, callFrame->codeBlock(), baseValue, subscript, value, byValInfo);
}


void JIT_OPERATION operationDirectPutByValGeneric(JSGlobalObject* globalObject, EncodedJSValue encodedBaseValue, EncodedJSValue encodedSubscript, EncodedJSValue encodedValue, ByValInfo* byValInfo)
{
    VM& vm = globalObject->vm();
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);
    
    JSValue baseValue = JSValue::decode(encodedBaseValue);
    JSValue subscript = JSValue::decode(encodedSubscript);
    JSValue value = JSValue::decode(encodedValue);
    RELEASE_ASSERT(baseValue.isObject());
    directPutByVal(globalObject, callFrame->codeBlock(), asObject(baseValue), subscript, value, byValInfo);
}

EncodedJSValue JIT_OPERATION operationCallEval(JSGlobalObject* globalObject, CallFrame* calleeFrame)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    calleeFrame->setCodeBlock(0);
    
    if (!isHostFunction(calleeFrame->guaranteedJSValueCallee(), globalFuncEval))
        return JSValue::encode(JSValue());

    JSValue result = eval(globalObject, calleeFrame);
    RETURN_IF_EXCEPTION(scope, encodedJSValue());
    
    return JSValue::encode(result);
}

static SlowPathReturnType handleHostCall(JSGlobalObject* globalObject, CallFrame* calleeFrame, JSValue callee, CallLinkInfo* callLinkInfo)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    calleeFrame->setCodeBlock(0);

    if (callLinkInfo->specializationKind() == CodeForCall) {
        CallData callData;
        CallType callType = getCallData(vm, callee, callData);
    
        ASSERT(callType != CallType::JS);
    
        if (callType == CallType::Host) {
            NativeCallFrameTracer tracer(vm, calleeFrame);
            calleeFrame->setCallee(asObject(callee));
            vm.hostCallReturnValue = JSValue::decode(callData.native.function(asObject(callee)->globalObject(vm), calleeFrame));
            if (UNLIKELY(scope.exception())) {
                return encodeResult(
                    vm.getCTIStub(throwExceptionFromCallSlowPathGenerator).retaggedCode<JSEntryPtrTag>().executableAddress(),
                    reinterpret_cast<void*>(KeepTheFrame));
            }

            return encodeResult(
                tagCFunctionPtr<void*, JSEntryPtrTag>(getHostCallReturnValue),
                reinterpret_cast<void*>(callLinkInfo->callMode() == CallMode::Tail ? ReuseTheFrame : KeepTheFrame));
        }
    
        ASSERT(callType == CallType::None);
        throwException(globalObject, scope, createNotAFunctionError(globalObject, callee));
        return encodeResult(
            vm.getCTIStub(throwExceptionFromCallSlowPathGenerator).retaggedCode<JSEntryPtrTag>().executableAddress(),
            reinterpret_cast<void*>(KeepTheFrame));
    }

    ASSERT(callLinkInfo->specializationKind() == CodeForConstruct);
    
    ConstructData constructData;
    ConstructType constructType = getConstructData(vm, callee, constructData);
    
    ASSERT(constructType != ConstructType::JS);
    
    if (constructType == ConstructType::Host) {
        NativeCallFrameTracer tracer(vm, calleeFrame);
        calleeFrame->setCallee(asObject(callee));
        vm.hostCallReturnValue = JSValue::decode(constructData.native.function(asObject(callee)->globalObject(vm), calleeFrame));
        if (UNLIKELY(scope.exception())) {
            return encodeResult(
                vm.getCTIStub(throwExceptionFromCallSlowPathGenerator).retaggedCode<JSEntryPtrTag>().executableAddress(),
                reinterpret_cast<void*>(KeepTheFrame));
        }

        return encodeResult(tagCFunctionPtr<void*, JSEntryPtrTag>(getHostCallReturnValue), reinterpret_cast<void*>(KeepTheFrame));
    }
    
    ASSERT(constructType == ConstructType::None);
    throwException(globalObject, scope, createNotAConstructorError(globalObject, callee));
    return encodeResult(
        vm.getCTIStub(throwExceptionFromCallSlowPathGenerator).retaggedCode<JSEntryPtrTag>().executableAddress(),
        reinterpret_cast<void*>(KeepTheFrame));
}

SlowPathReturnType JIT_OPERATION operationLinkCall(CallFrame* calleeFrame, JSGlobalObject* globalObject, CallLinkInfo* callLinkInfo)
{
    CallFrame* callFrame = calleeFrame->callerFrame();
    VM& vm = globalObject->vm();
    auto throwScope = DECLARE_THROW_SCOPE(vm);

    CodeSpecializationKind kind = callLinkInfo->specializationKind();
    NativeCallFrameTracer tracer(vm, callFrame);
    
    RELEASE_ASSERT(!callLinkInfo->isDirect());
    
    JSValue calleeAsValue = calleeFrame->guaranteedJSValueCallee();
    JSCell* calleeAsFunctionCell = getJSFunction(calleeAsValue);
    if (!calleeAsFunctionCell) {
        if (auto* internalFunction = jsDynamicCast<InternalFunction*>(vm, calleeAsValue)) {
            MacroAssemblerCodePtr<JSEntryPtrTag> codePtr = vm.getCTIInternalFunctionTrampolineFor(kind);
            RELEASE_ASSERT(!!codePtr);

            if (!callLinkInfo->seenOnce())
                callLinkInfo->setSeen();
            else
                linkFor(vm, calleeFrame, *callLinkInfo, nullptr, internalFunction, codePtr);

            void* linkedTarget = codePtr.executableAddress();
            return encodeResult(linkedTarget, reinterpret_cast<void*>(callLinkInfo->callMode() == CallMode::Tail ? ReuseTheFrame : KeepTheFrame));
        }
        RELEASE_AND_RETURN(throwScope, handleHostCall(globalObject, calleeFrame, calleeAsValue, callLinkInfo));
    }

    JSFunction* callee = jsCast<JSFunction*>(calleeAsFunctionCell);
    JSScope* scope = callee->scopeUnchecked();
    ExecutableBase* executable = callee->executable();

    MacroAssemblerCodePtr<JSEntryPtrTag> codePtr;
    CodeBlock* codeBlock = nullptr;
    if (executable->isHostFunction()) {
        codePtr = jsToWasmICCodePtr(vm, kind, callee);
        if (!codePtr)
            codePtr = executable->entrypointFor(kind, MustCheckArity);
    } else {
        FunctionExecutable* functionExecutable = static_cast<FunctionExecutable*>(executable);

        auto handleThrowException = [&] () {
            void* throwTarget = vm.getCTIStub(throwExceptionFromCallSlowPathGenerator).retaggedCode<JSEntryPtrTag>().executableAddress();
            return encodeResult(throwTarget, reinterpret_cast<void*>(KeepTheFrame));
        };

        if (!isCall(kind) && functionExecutable->constructAbility() == ConstructAbility::CannotConstruct) {
            throwException(globalObject, throwScope, createNotAConstructorError(globalObject, callee));
            return handleThrowException();
        }

        CodeBlock** codeBlockSlot = calleeFrame->addressOfCodeBlock();
        Exception* error = functionExecutable->prepareForExecution<FunctionExecutable>(vm, callee, scope, kind, *codeBlockSlot);
        EXCEPTION_ASSERT(throwScope.exception() == error);
        if (UNLIKELY(error))
            return handleThrowException();
        codeBlock = *codeBlockSlot;
        ArityCheckMode arity;
        if (calleeFrame->argumentCountIncludingThis() < static_cast<size_t>(codeBlock->numParameters()) || callLinkInfo->isVarargs())
            arity = MustCheckArity;
        else
            arity = ArityCheckNotRequired;
        codePtr = functionExecutable->entrypointFor(kind, arity);
    }

    if (!callLinkInfo->seenOnce())
        callLinkInfo->setSeen();
    else
        linkFor(vm, calleeFrame, *callLinkInfo, codeBlock, callee, codePtr);

    return encodeResult(codePtr.executableAddress(), reinterpret_cast<void*>(callLinkInfo->callMode() == CallMode::Tail ? ReuseTheFrame : KeepTheFrame));
}

inline SlowPathReturnType virtualForWithFunction(JSGlobalObject* globalObject, CallFrame* calleeFrame, CallLinkInfo* callLinkInfo, JSCell*& calleeAsFunctionCell)
{
    CallFrame* callFrame = calleeFrame->callerFrame();
    VM& vm = globalObject->vm();
    auto throwScope = DECLARE_THROW_SCOPE(vm);

    CodeSpecializationKind kind = callLinkInfo->specializationKind();
    NativeCallFrameTracer tracer(vm, callFrame);

    JSValue calleeAsValue = calleeFrame->guaranteedJSValueCallee();
    calleeAsFunctionCell = getJSFunction(calleeAsValue);
    if (UNLIKELY(!calleeAsFunctionCell)) {
        if (jsDynamicCast<InternalFunction*>(vm, calleeAsValue)) {
            MacroAssemblerCodePtr<JSEntryPtrTag> codePtr = vm.getCTIInternalFunctionTrampolineFor(kind);
            ASSERT(!!codePtr);
            return encodeResult(codePtr.executableAddress(), reinterpret_cast<void*>(callLinkInfo->callMode() == CallMode::Tail ? ReuseTheFrame : KeepTheFrame));
        }
        RELEASE_AND_RETURN(throwScope, handleHostCall(globalObject, calleeFrame, calleeAsValue, callLinkInfo));
    }
    
    JSFunction* function = jsCast<JSFunction*>(calleeAsFunctionCell);
    JSScope* scope = function->scopeUnchecked();
    ExecutableBase* executable = function->executable();
    if (UNLIKELY(!executable->hasJITCodeFor(kind))) {
        FunctionExecutable* functionExecutable = static_cast<FunctionExecutable*>(executable);

        if (!isCall(kind) && functionExecutable->constructAbility() == ConstructAbility::CannotConstruct) {
            throwException(globalObject, throwScope, createNotAConstructorError(globalObject, function));
            return encodeResult(
                vm.getCTIStub(throwExceptionFromCallSlowPathGenerator).retaggedCode<JSEntryPtrTag>().executableAddress(),
                reinterpret_cast<void*>(KeepTheFrame));
        }

        CodeBlock** codeBlockSlot = calleeFrame->addressOfCodeBlock();
        Exception* error = functionExecutable->prepareForExecution<FunctionExecutable>(vm, function, scope, kind, *codeBlockSlot);
        EXCEPTION_ASSERT(throwScope.exception() == error);
        if (UNLIKELY(error)) {
            return encodeResult(
                vm.getCTIStub(throwExceptionFromCallSlowPathGenerator).retaggedCode<JSEntryPtrTag>().executableAddress(),
                reinterpret_cast<void*>(KeepTheFrame));
        }
    }
    return encodeResult(executable->entrypointFor(
        kind, MustCheckArity).executableAddress(),
        reinterpret_cast<void*>(callLinkInfo->callMode() == CallMode::Tail ? ReuseTheFrame : KeepTheFrame));
}

SlowPathReturnType JIT_OPERATION operationLinkPolymorphicCall(CallFrame* calleeFrame, JSGlobalObject* globalObject, CallLinkInfo* callLinkInfo)
{
    ASSERT(callLinkInfo->specializationKind() == CodeForCall);
    JSCell* calleeAsFunctionCell;
    SlowPathReturnType result = virtualForWithFunction(globalObject, calleeFrame, callLinkInfo, calleeAsFunctionCell);

    linkPolymorphicCall(globalObject, calleeFrame, *callLinkInfo, CallVariant(calleeAsFunctionCell));
    
    return result;
}

SlowPathReturnType JIT_OPERATION operationVirtualCall(CallFrame* calleeFrame, JSGlobalObject* globalObject, CallLinkInfo* callLinkInfo)
{
    JSCell* calleeAsFunctionCellIgnored;
    return virtualForWithFunction(globalObject, calleeFrame, callLinkInfo, calleeAsFunctionCellIgnored);
}

size_t JIT_OPERATION operationCompareLess(JSGlobalObject* globalObject, EncodedJSValue encodedOp1, EncodedJSValue encodedOp2)
{
    VM& vm = globalObject->vm();
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);
    
    return jsLess<true>(globalObject, JSValue::decode(encodedOp1), JSValue::decode(encodedOp2));
}

size_t JIT_OPERATION operationCompareLessEq(JSGlobalObject* globalObject, EncodedJSValue encodedOp1, EncodedJSValue encodedOp2)
{
    VM& vm = globalObject->vm();
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);

    return jsLessEq<true>(globalObject, JSValue::decode(encodedOp1), JSValue::decode(encodedOp2));
}

size_t JIT_OPERATION operationCompareGreater(JSGlobalObject* globalObject, EncodedJSValue encodedOp1, EncodedJSValue encodedOp2)
{
    VM& vm = globalObject->vm();
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);

    return jsLess<false>(globalObject, JSValue::decode(encodedOp2), JSValue::decode(encodedOp1));
}

size_t JIT_OPERATION operationCompareGreaterEq(JSGlobalObject* globalObject, EncodedJSValue encodedOp1, EncodedJSValue encodedOp2)
{
    VM& vm = globalObject->vm();
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);

    return jsLessEq<false>(globalObject, JSValue::decode(encodedOp2), JSValue::decode(encodedOp1));
}

size_t JIT_OPERATION operationCompareEq(JSGlobalObject* globalObject, EncodedJSValue encodedOp1, EncodedJSValue encodedOp2)
{
    VM& vm = globalObject->vm();
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);

    return JSValue::equalSlowCaseInline(globalObject, JSValue::decode(encodedOp1), JSValue::decode(encodedOp2));
}

#if USE(JSVALUE64)
EncodedJSValue JIT_OPERATION operationCompareStringEq(JSGlobalObject* globalObject, JSCell* left, JSCell* right)
#else
size_t JIT_OPERATION operationCompareStringEq(JSGlobalObject* globalObject, JSCell* left, JSCell* right)
#endif
{
    VM& vm = globalObject->vm();
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);

    bool result = asString(left)->equal(globalObject, asString(right));
#if USE(JSVALUE64)
    return JSValue::encode(jsBoolean(result));
#else
    return result;
#endif
}

size_t JIT_OPERATION operationCompareStrictEq(JSGlobalObject* globalObject, EncodedJSValue encodedOp1, EncodedJSValue encodedOp2)
{
    VM& vm = globalObject->vm();
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);

    JSValue src1 = JSValue::decode(encodedOp1);
    JSValue src2 = JSValue::decode(encodedOp2);

    return JSValue::strictEqual(globalObject, src1, src2);
}

EncodedJSValue JIT_OPERATION operationNewArrayWithProfile(JSGlobalObject* globalObject, ArrayAllocationProfile* profile, const JSValue* values, int size)
{
    VM& vm = globalObject->vm();
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);
    return JSValue::encode(constructArrayNegativeIndexed(globalObject, profile, values, size));
}

EncodedJSValue JIT_OPERATION operationNewArrayWithSizeAndProfile(JSGlobalObject* globalObject, ArrayAllocationProfile* profile, EncodedJSValue size)
{
    VM& vm = globalObject->vm();
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);
    JSValue sizeValue = JSValue::decode(size);
    return JSValue::encode(constructArrayWithSizeQuirk(globalObject, profile, sizeValue));
}

}

template<typename FunctionType>
static EncodedJSValue newFunctionCommon(VM& vm, JSScope* scope, JSCell* functionExecutable, bool isInvalidated)
{
    ASSERT(functionExecutable->inherits<FunctionExecutable>(vm));
    if (isInvalidated)
        return JSValue::encode(FunctionType::createWithInvalidatedReallocationWatchpoint(vm, static_cast<FunctionExecutable*>(functionExecutable), scope));
    return JSValue::encode(FunctionType::create(vm, static_cast<FunctionExecutable*>(functionExecutable), scope));
}

extern "C" {

EncodedJSValue JIT_OPERATION operationNewFunction(VM* vmPointer, JSScope* scope, JSCell* functionExecutable)
{
    VM& vm = *vmPointer;
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);
    return newFunctionCommon<JSFunction>(vm, scope, functionExecutable, false);
}

EncodedJSValue JIT_OPERATION operationNewFunctionWithInvalidatedReallocationWatchpoint(VM* vmPointer, JSScope* scope, JSCell* functionExecutable)
{
    VM& vm = *vmPointer;
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);
    return newFunctionCommon<JSFunction>(vm, scope, functionExecutable, true);
}

EncodedJSValue JIT_OPERATION operationNewGeneratorFunction(VM* vmPointer, JSScope* scope, JSCell* functionExecutable)
{
    VM& vm = *vmPointer;
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);
    return newFunctionCommon<JSGeneratorFunction>(vm, scope, functionExecutable, false);
}

EncodedJSValue JIT_OPERATION operationNewGeneratorFunctionWithInvalidatedReallocationWatchpoint(VM* vmPointer, JSScope* scope, JSCell* functionExecutable)
{
    VM& vm = *vmPointer;
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);
    return newFunctionCommon<JSGeneratorFunction>(vm, scope, functionExecutable, true);
}

EncodedJSValue JIT_OPERATION operationNewAsyncFunction(VM* vmPointer, JSScope* scope, JSCell* functionExecutable)
{
    VM& vm = *vmPointer;
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);
    return newFunctionCommon<JSAsyncFunction>(vm, scope, functionExecutable, false);
}

EncodedJSValue JIT_OPERATION operationNewAsyncFunctionWithInvalidatedReallocationWatchpoint(VM* vmPointer, JSScope* scope, JSCell* functionExecutable)
{
    VM& vm = *vmPointer;
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);
    return newFunctionCommon<JSAsyncFunction>(vm, scope, functionExecutable, true);
}

EncodedJSValue JIT_OPERATION operationNewAsyncGeneratorFunction(VM* vmPointer, JSScope* scope, JSCell* functionExecutable)
{
    VM& vm = *vmPointer;
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);
    return newFunctionCommon<JSAsyncGeneratorFunction>(vm, scope, functionExecutable, false);
}
    
EncodedJSValue JIT_OPERATION operationNewAsyncGeneratorFunctionWithInvalidatedReallocationWatchpoint(VM* vmPointer, JSScope* scope, JSCell* functionExecutable)
{
    VM& vm = *vmPointer;
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);
    return newFunctionCommon<JSAsyncGeneratorFunction>(vm, scope, functionExecutable, true);
}
    
void JIT_OPERATION operationSetFunctionName(JSGlobalObject* globalObject, JSCell* funcCell, EncodedJSValue encodedName)
{
    VM& vm = globalObject->vm();
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);

    JSFunction* func = jsCast<JSFunction*>(funcCell);
    JSValue name = JSValue::decode(encodedName);
    func->setFunctionName(globalObject, name);
}

JSCell* JIT_OPERATION operationNewObject(VM* vmPointer, Structure* structure)
{
    VM& vm = *vmPointer;
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);

    return constructEmptyObject(vm, structure);
}

JSCell* JIT_OPERATION operationNewPromise(VM* vmPointer, Structure* structure)
{
    VM& vm = *vmPointer;
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);

    return JSPromise::create(vm, structure);
}

JSCell* JIT_OPERATION operationNewInternalPromise(VM* vmPointer, Structure* structure)
{
    VM& vm = *vmPointer;
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);

    return JSInternalPromise::create(vm, structure);
}

JSCell* JIT_OPERATION operationNewGenerator(VM* vmPointer, Structure* structure)
{
    VM& vm = *vmPointer;
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);

    return JSGenerator::create(vm, structure);
}

JSCell* JIT_OPERATION operationNewAsyncGenerator(VM* vmPointer, Structure* structure)
{
    VM& vm = *vmPointer;
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);

    return JSAsyncGenerator::create(vm, structure);
}

JSCell* JIT_OPERATION operationNewRegexp(JSGlobalObject* globalObject, JSCell* regexpPtr)
{
    SuperSamplerScope superSamplerScope(false);
    VM& vm = globalObject->vm();
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);

    RegExp* regexp = static_cast<RegExp*>(regexpPtr);
    ASSERT(regexp->isValid());
    return RegExpObject::create(vm, globalObject->regExpStructure(), regexp);
}

// The only reason for returning an UnusedPtr (instead of void) is so that we can reuse the
// existing DFG slow path generator machinery when creating the slow path for CheckTraps
// in the DFG. If a DFG slow path generator that supports a void return type is added in the
// future, we can switch to using that then.
UnusedPtr JIT_OPERATION operationHandleTraps(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);
    ASSERT(vm.needTrapHandling());
    vm.handleTraps(globalObject, callFrame);
    return nullptr;
}

void JIT_OPERATION operationDebug(VM* vmPointer, int32_t debugHookType)
{
    VM& vm = *vmPointer;
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);

    vm.interpreter->debug(callFrame, static_cast<DebugHookType>(debugHookType));
}

#if ENABLE(DFG_JIT)
static void updateAllPredictionsAndOptimizeAfterWarmUp(CodeBlock* codeBlock)
{
    codeBlock->updateAllPredictions();
    codeBlock->optimizeAfterWarmUp();
}

SlowPathReturnType JIT_OPERATION operationOptimize(VM* vmPointer, uint32_t bytecodeIndexBits)
{
    VM& vm = *vmPointer;
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);
    BytecodeIndex bytecodeIndex = BytecodeIndex::fromBits(bytecodeIndexBits);

    // Defer GC for a while so that it doesn't run between when we enter into this
    // slow path and when we figure out the state of our code block. This prevents
    // a number of awkward reentrancy scenarios, including:
    //
    // - The optimized version of our code block being jettisoned by GC right after
    //   we concluded that we wanted to use it, but have not planted it into the JS
    //   stack yet.
    //
    // - An optimized version of our code block being installed just as we decided
    //   that it wasn't ready yet.
    //
    // Note that jettisoning won't happen if we already initiated OSR, because in
    // that case we would have already planted the optimized code block into the JS
    // stack.
    DeferGCForAWhile deferGC(vm.heap);
    
    CodeBlock* codeBlock = callFrame->codeBlock();
    if (UNLIKELY(codeBlock->jitType() != JITType::BaselineJIT)) {
        dataLog("Unexpected code block in Baseline->DFG tier-up: ", *codeBlock, "\n");
        RELEASE_ASSERT_NOT_REACHED();
    }
    
    if (bytecodeIndex) {
        // If we're attempting to OSR from a loop, assume that this should be
        // separately optimized.
        codeBlock->m_shouldAlwaysBeInlined = false;
    }

    if (UNLIKELY(Options::verboseOSR())) {
        dataLog(
            *codeBlock, ": Entered optimize with bytecodeIndex = ", bytecodeIndex,
            ", executeCounter = ", codeBlock->jitExecuteCounter(),
            ", optimizationDelayCounter = ", codeBlock->reoptimizationRetryCounter(),
            ", exitCounter = ");
        if (codeBlock->hasOptimizedReplacement())
            dataLog(codeBlock->replacement()->osrExitCounter());
        else
            dataLog("N/A");
        dataLog("\n");
    }

    if (!codeBlock->checkIfOptimizationThresholdReached()) {
        CODEBLOCK_LOG_EVENT(codeBlock, "delayOptimizeToDFG", ("counter = ", codeBlock->jitExecuteCounter()));
        codeBlock->updateAllPredictions();
        if (UNLIKELY(Options::verboseOSR()))
            dataLog("Choosing not to optimize ", *codeBlock, " yet, because the threshold hasn't been reached.\n");
        return encodeResult(0, 0);
    }
    
    Debugger* debugger = codeBlock->globalObject()->debugger();
    if (UNLIKELY(debugger && (debugger->isStepping() || codeBlock->baselineAlternative()->hasDebuggerRequests()))) {
        CODEBLOCK_LOG_EVENT(codeBlock, "delayOptimizeToDFG", ("debugger is stepping or has requests"));
        updateAllPredictionsAndOptimizeAfterWarmUp(codeBlock);
        return encodeResult(0, 0);
    }

    if (codeBlock->m_shouldAlwaysBeInlined) {
        CODEBLOCK_LOG_EVENT(codeBlock, "delayOptimizeToDFG", ("should always be inlined"));
        updateAllPredictionsAndOptimizeAfterWarmUp(codeBlock);
        if (UNLIKELY(Options::verboseOSR()))
            dataLog("Choosing not to optimize ", *codeBlock, " yet, because m_shouldAlwaysBeInlined == true.\n");
        return encodeResult(0, 0);
    }

    // We cannot be in the process of asynchronous compilation and also have an optimized
    // replacement.
    DFG::Worklist* worklist = DFG::existingGlobalDFGWorklistOrNull();
    ASSERT(
        !worklist
        || !(worklist->compilationState(DFG::CompilationKey(codeBlock, DFG::DFGMode)) != DFG::Worklist::NotKnown
        && codeBlock->hasOptimizedReplacement()));

    DFG::Worklist::State worklistState;
    if (worklist) {
        // The call to DFG::Worklist::completeAllReadyPlansForVM() will complete all ready
        // (i.e. compiled) code blocks. But if it completes ours, we also need to know
        // what the result was so that we don't plow ahead and attempt OSR or immediate
        // reoptimization. This will have already also set the appropriate JIT execution
        // count threshold depending on what happened, so if the compilation was anything
        // but successful we just want to return early. See the case for worklistState ==
        // DFG::Worklist::Compiled, below.
        
        // Note that we could have alternatively just called Worklist::compilationState()
        // here, and if it returned Compiled, we could have then called
        // completeAndScheduleOSR() below. But that would have meant that it could take
        // longer for code blocks to be completed: they would only complete when *their*
        // execution count trigger fired; but that could take a while since the firing is
        // racy. It could also mean that code blocks that never run again after being
        // compiled would sit on the worklist until next GC. That's fine, but it's
        // probably a waste of memory. Our goal here is to complete code blocks as soon as
        // possible in order to minimize the chances of us executing baseline code after
        // optimized code is already available.
        worklistState = worklist->completeAllReadyPlansForVM(
            vm, DFG::CompilationKey(codeBlock, DFG::DFGMode));
    } else
        worklistState = DFG::Worklist::NotKnown;

    if (worklistState == DFG::Worklist::Compiling) {
        CODEBLOCK_LOG_EVENT(codeBlock, "delayOptimizeToDFG", ("compiling"));
        // We cannot be in the process of asynchronous compilation and also have an optimized
        // replacement.
        RELEASE_ASSERT(!codeBlock->hasOptimizedReplacement());
        codeBlock->setOptimizationThresholdBasedOnCompilationResult(CompilationDeferred);
        return encodeResult(0, 0);
    }

    if (worklistState == DFG::Worklist::Compiled) {
        // If we don't have an optimized replacement but we did just get compiled, then
        // the compilation failed or was invalidated, in which case the execution count
        // thresholds have already been set appropriately by
        // CodeBlock::setOptimizationThresholdBasedOnCompilationResult() and we have
        // nothing left to do.
        if (!codeBlock->hasOptimizedReplacement()) {
            CODEBLOCK_LOG_EVENT(codeBlock, "delayOptimizeToDFG", ("compiled and failed"));
            codeBlock->updateAllPredictions();
            if (UNLIKELY(Options::verboseOSR()))
                dataLog("Code block ", *codeBlock, " was compiled but it doesn't have an optimized replacement.\n");
            return encodeResult(0, 0);
        }
    } else if (codeBlock->hasOptimizedReplacement()) {
        CodeBlock* replacement = codeBlock->replacement();
        if (UNLIKELY(Options::verboseOSR()))
            dataLog("Considering OSR ", codeBlock, " -> ", replacement, ".\n");
        // If we have an optimized replacement, then it must be the case that we entered
        // cti_optimize from a loop. That's because if there's an optimized replacement,
        // then all calls to this function will be relinked to the replacement and so
        // the prologue OSR will never fire.
        
        // This is an interesting threshold check. Consider that a function OSR exits
        // in the middle of a loop, while having a relatively low exit count. The exit
        // will reset the execution counter to some target threshold, meaning that this
        // code won't be reached until that loop heats up for >=1000 executions. But then
        // we do a second check here, to see if we should either reoptimize, or just
        // attempt OSR entry. Hence it might even be correct for
        // shouldReoptimizeFromLoopNow() to always return true. But we make it do some
        // additional checking anyway, to reduce the amount of recompilation thrashing.
        if (replacement->shouldReoptimizeFromLoopNow()) {
            CODEBLOCK_LOG_EVENT(codeBlock, "delayOptimizeToDFG", ("should reoptimize from loop now"));
            if (UNLIKELY(Options::verboseOSR())) {
                dataLog(
                    "Triggering reoptimization of ", codeBlock,
                    "(", replacement, ") (in loop).\n");
            }
            replacement->jettison(Profiler::JettisonDueToBaselineLoopReoptimizationTrigger, CountReoptimization);
            return encodeResult(0, 0);
        }
    } else {
        if (!codeBlock->shouldOptimizeNow()) {
            CODEBLOCK_LOG_EVENT(codeBlock, "delayOptimizeToDFG", ("insufficient profiling"));
            if (UNLIKELY(Options::verboseOSR())) {
                dataLog(
                    "Delaying optimization for ", *codeBlock,
                    " because of insufficient profiling.\n");
            }
            return encodeResult(0, 0);
        }

        if (UNLIKELY(Options::verboseOSR()))
            dataLog("Triggering optimized compilation of ", *codeBlock, "\n");

        unsigned numVarsWithValues;
        if (bytecodeIndex)
            numVarsWithValues = codeBlock->numCalleeLocals();
        else
            numVarsWithValues = 0;
        Operands<Optional<JSValue>> mustHandleValues(codeBlock->numParameters(), numVarsWithValues);
        int localsUsedForCalleeSaves = static_cast<int>(CodeBlock::llintBaselineCalleeSaveSpaceAsVirtualRegisters());
        for (size_t i = 0; i < mustHandleValues.size(); ++i) {
            int operand = mustHandleValues.operandForIndex(i);
            if (operandIsLocal(operand) && VirtualRegister(operand).toLocal() < localsUsedForCalleeSaves)
                continue;
            mustHandleValues[i] = callFrame->uncheckedR(operand).jsValue();
        }

        CodeBlock* replacementCodeBlock = codeBlock->newReplacement();
        CompilationResult result = DFG::compile(
            vm, replacementCodeBlock, nullptr, DFG::DFGMode, bytecodeIndex,
            mustHandleValues, JITToDFGDeferredCompilationCallback::create());
        
        if (result != CompilationSuccessful) {
            CODEBLOCK_LOG_EVENT(codeBlock, "delayOptimizeToDFG", ("compilation failed"));
            return encodeResult(0, 0);
        }
    }
    
    CodeBlock* optimizedCodeBlock = codeBlock->replacement();
    ASSERT(optimizedCodeBlock && JITCode::isOptimizingJIT(optimizedCodeBlock->jitType()));
    
    if (void* dataBuffer = DFG::prepareOSREntry(vm, callFrame, optimizedCodeBlock, bytecodeIndex)) {
        CODEBLOCK_LOG_EVENT(optimizedCodeBlock, "osrEntry", ("at bc#", bytecodeIndex));
        if (UNLIKELY(Options::verboseOSR())) {
            dataLog(
                "Performing OSR ", codeBlock, " -> ", optimizedCodeBlock, ".\n");
        }

        codeBlock->optimizeSoon();
        codeBlock->unlinkedCodeBlock()->setDidOptimize(TrueTriState);
        void* targetPC = vm.getCTIStub(DFG::osrEntryThunkGenerator).code().executableAddress();
        targetPC = retagCodePtr(targetPC, JITThunkPtrTag, bitwise_cast<PtrTag>(callFrame));
        return encodeResult(targetPC, dataBuffer);
    }

    if (UNLIKELY(Options::verboseOSR())) {
        dataLog(
            "Optimizing ", codeBlock, " -> ", codeBlock->replacement(),
            " succeeded, OSR failed, after a delay of ",
            codeBlock->optimizationDelayCounter(), ".\n");
    }

    // Count the OSR failure as a speculation failure. If this happens a lot, then
    // reoptimize.
    optimizedCodeBlock->countOSRExit();

    // We are a lot more conservative about triggering reoptimization after OSR failure than
    // before it. If we enter the optimize_from_loop trigger with a bucket full of fail
    // already, then we really would like to reoptimize immediately. But this case covers
    // something else: there weren't many (or any) speculation failures before, but we just
    // failed to enter the speculative code because some variable had the wrong value or
    // because the OSR code decided for any spurious reason that it did not want to OSR
    // right now. So, we only trigger reoptimization only upon the more conservative (non-loop)
    // reoptimization trigger.
    if (optimizedCodeBlock->shouldReoptimizeNow()) {
        CODEBLOCK_LOG_EVENT(codeBlock, "delayOptimizeToDFG", ("should reoptimize now"));
        if (UNLIKELY(Options::verboseOSR())) {
            dataLog(
                "Triggering reoptimization of ", codeBlock, " -> ",
                codeBlock->replacement(), " (after OSR fail).\n");
        }
        optimizedCodeBlock->jettison(Profiler::JettisonDueToBaselineLoopReoptimizationTriggerOnOSREntryFail, CountReoptimization);
        return encodeResult(0, 0);
    }

    // OSR failed this time, but it might succeed next time! Let the code run a bit
    // longer and then try again.
    codeBlock->optimizeAfterWarmUp();
    
    CODEBLOCK_LOG_EVENT(codeBlock, "delayOptimizeToDFG", ("OSR failed"));
    return encodeResult(0, 0);
}

char* JIT_OPERATION operationTryOSREnterAtCatch(VM* vmPointer, uint32_t bytecodeIndexBits)
{
    VM& vm = *vmPointer;
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);
    BytecodeIndex bytecodeIndex = BytecodeIndex::fromBits(bytecodeIndexBits);

    CodeBlock* codeBlock = callFrame->codeBlock();
    CodeBlock* optimizedReplacement = codeBlock->replacement();
    if (UNLIKELY(!optimizedReplacement))
        return nullptr;

    switch (optimizedReplacement->jitType()) {
    case JITType::DFGJIT:
    case JITType::FTLJIT: {
        MacroAssemblerCodePtr<ExceptionHandlerPtrTag> entry = DFG::prepareCatchOSREntry(vm, callFrame, codeBlock, optimizedReplacement, bytecodeIndex);
        return entry.executableAddress<char*>();
    }
    default:
        break;
    }
    return nullptr;
}

char* JIT_OPERATION operationTryOSREnterAtCatchAndValueProfile(VM* vmPointer, uint32_t bytecodeIndexBits)
{
    VM& vm = *vmPointer;
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);
    BytecodeIndex bytecodeIndex = BytecodeIndex::fromBits(bytecodeIndexBits);

    CodeBlock* codeBlock = callFrame->codeBlock();
    CodeBlock* optimizedReplacement = codeBlock->replacement();
    if (UNLIKELY(!optimizedReplacement))
        return nullptr;

    switch (optimizedReplacement->jitType()) {
    case JITType::DFGJIT:
    case JITType::FTLJIT: {
        MacroAssemblerCodePtr<ExceptionHandlerPtrTag> entry = DFG::prepareCatchOSREntry(vm, callFrame, codeBlock, optimizedReplacement, bytecodeIndex);
        return entry.executableAddress<char*>();
    }
    default:
        break;
    }

    codeBlock->ensureCatchLivenessIsComputedForBytecodeIndex(bytecodeIndex);
    auto bytecode = codeBlock->instructions().at(bytecodeIndex)->as<OpCatch>();
    auto& metadata = bytecode.metadata(codeBlock);
    metadata.m_buffer->forEach([&] (ValueProfileAndOperand& profile) {
        profile.m_buckets[0] = JSValue::encode(callFrame->uncheckedR(profile.m_operand).jsValue());
    });

    return nullptr;
}

#endif

void JIT_OPERATION operationPutByIndex(JSGlobalObject* globalObject, EncodedJSValue encodedArrayValue, int32_t index, EncodedJSValue encodedValue)
{
    VM& vm = globalObject->vm();
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);

    JSValue arrayValue = JSValue::decode(encodedArrayValue);
    ASSERT(isJSArray(arrayValue));
    asArray(arrayValue)->putDirectIndex(globalObject, index, JSValue::decode(encodedValue));
}

enum class AccessorType {
    Getter,
    Setter
};

static void putAccessorByVal(JSGlobalObject* globalObject, JSObject* base, JSValue subscript, int32_t attribute, JSObject* accessor, AccessorType accessorType)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto propertyKey = subscript.toPropertyKey(globalObject);
    RETURN_IF_EXCEPTION(scope, void());

    scope.release();
    if (accessorType == AccessorType::Getter)
        base->putGetter(globalObject, propertyKey, accessor, attribute);
    else
        base->putSetter(globalObject, propertyKey, accessor, attribute);
}

void JIT_OPERATION operationPutGetterById(JSGlobalObject* globalObject, JSCell* object, UniquedStringImpl* uid, int32_t options, JSCell* getter)
{
    VM& vm = globalObject->vm();
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);

    ASSERT(object && object->isObject());
    JSObject* baseObj = object->getObject();

    ASSERT(getter->isObject());
    baseObj->putGetter(globalObject, uid, getter, options);
}

void JIT_OPERATION operationPutSetterById(JSGlobalObject* globalObject, JSCell* object, UniquedStringImpl* uid, int32_t options, JSCell* setter)
{
    VM& vm = globalObject->vm();
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);

    ASSERT(object && object->isObject());
    JSObject* baseObj = object->getObject();

    ASSERT(setter->isObject());
    baseObj->putSetter(globalObject, uid, setter, options);
}

void JIT_OPERATION operationPutGetterByVal(JSGlobalObject* globalObject, JSCell* base, EncodedJSValue encodedSubscript, int32_t attribute, JSCell* getter)
{
    VM& vm = globalObject->vm();
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);

    putAccessorByVal(globalObject, asObject(base), JSValue::decode(encodedSubscript), attribute, asObject(getter), AccessorType::Getter);
}

void JIT_OPERATION operationPutSetterByVal(JSGlobalObject* globalObject, JSCell* base, EncodedJSValue encodedSubscript, int32_t attribute, JSCell* setter)
{
    VM& vm = globalObject->vm();
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);

    putAccessorByVal(globalObject, asObject(base), JSValue::decode(encodedSubscript), attribute, asObject(setter), AccessorType::Setter);
}

#if USE(JSVALUE64)
void JIT_OPERATION operationPutGetterSetter(JSGlobalObject* globalObject, JSCell* object, UniquedStringImpl* uid, int32_t attribute, EncodedJSValue encodedGetterValue, EncodedJSValue encodedSetterValue)
{
    VM& vm = globalObject->vm();
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);

    ASSERT(object && object->isObject());
    JSObject* baseObject = asObject(object);

    JSValue getter = JSValue::decode(encodedGetterValue);
    JSValue setter = JSValue::decode(encodedSetterValue);
    ASSERT(getter.isObject() || setter.isObject());
    GetterSetter* accessor = GetterSetter::create(vm, globalObject, getter, setter);
    CommonSlowPaths::putDirectAccessorWithReify(vm, globalObject, baseObject, uid, accessor, attribute);
}

#else
void JIT_OPERATION operationPutGetterSetter(JSGlobalObject* globalObject, JSCell* object, UniquedStringImpl* uid, int32_t attribute, JSCell* getterCell, JSCell* setterCell)
{
    VM& vm = globalObject->vm();
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);

    ASSERT(object && object->isObject());
    JSObject* baseObject = asObject(object);

    ASSERT(getterCell || setterCell);
    JSObject* getter = getterCell ? getterCell->getObject() : nullptr;
    JSObject* setter = setterCell ? setterCell->getObject() : nullptr;
    GetterSetter* accessor = GetterSetter::create(vm, globalObject, getter, setter);
    CommonSlowPaths::putDirectAccessorWithReify(vm, globalObject, baseObject, uid, accessor, attribute);
}
#endif

void JIT_OPERATION operationPopScope(JSGlobalObject* globalObject, int32_t scopeReg)
{
    VM& vm = globalObject->vm();
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);

    JSScope* scope = callFrame->uncheckedR(scopeReg).Register::scope();
    callFrame->uncheckedR(scopeReg) = scope->next();
}

int32_t JIT_OPERATION operationInstanceOfCustom(JSGlobalObject* globalObject, EncodedJSValue encodedValue, JSObject* constructor, EncodedJSValue encodedHasInstance)
{
    VM& vm = globalObject->vm();
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);

    JSValue value = JSValue::decode(encodedValue);
    JSValue hasInstanceValue = JSValue::decode(encodedHasInstance);

    if (constructor->hasInstance(globalObject, value, hasInstanceValue))
        return 1;
    return 0;
}

}

ALWAYS_INLINE static JSValue getByVal(JSGlobalObject* globalObject, CallFrame* callFrame, ArrayProfile* arrayProfile, JSValue baseValue, JSValue subscript)
{
    UNUSED_PARAM(callFrame);
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    if (LIKELY(baseValue.isCell() && subscript.isString())) {
        Structure& structure = *baseValue.asCell()->structure(vm);
        if (JSCell::canUseFastGetOwnProperty(structure)) {
            RefPtr<AtomStringImpl> existingAtomString = asString(subscript)->toExistingAtomString(globalObject);
            RETURN_IF_EXCEPTION(scope, JSValue());
            if (existingAtomString) {
                if (JSValue result = baseValue.asCell()->fastGetOwnProperty(vm, structure, existingAtomString.get())) {
                    ASSERT(callFrame->bytecodeIndex() != BytecodeIndex(0));
                    return result;
                }
            }
        }
    }

    if (subscript.isInt32()) {
        int32_t i = subscript.asInt32();
        if (isJSString(baseValue)) {
            if (i >= 0 && asString(baseValue)->canGetIndex(i))
                RELEASE_AND_RETURN(scope, asString(baseValue)->getIndex(globalObject, i));
            if (arrayProfile)
                arrayProfile->setOutOfBounds();
        } else if (baseValue.isObject()) {
            JSObject* object = asObject(baseValue);
            if (object->canGetIndexQuickly(i))
                return object->getIndexQuickly(i);

            bool skipMarkingOutOfBounds = false;

            if (object->indexingType() == ArrayWithContiguous && i >= 0 && static_cast<uint32_t>(i) < object->butterfly()->publicLength()) {
                // FIXME: expand this to ArrayStorage, Int32, and maybe Double:
                // https://bugs.webkit.org/show_bug.cgi?id=182940
                auto* globalObject = object->globalObject(vm);
                skipMarkingOutOfBounds = globalObject->isOriginalArrayStructure(object->structure(vm)) && globalObject->arrayPrototypeChainIsSane();
            }

            if (!skipMarkingOutOfBounds && !CommonSlowPaths::canAccessArgumentIndexQuickly(*object, i)) {
                // FIXME: This will make us think that in-bounds typed array accesses are actually
                // out-of-bounds.
                // https://bugs.webkit.org/show_bug.cgi?id=149886
                if (arrayProfile)
                    arrayProfile->setOutOfBounds();
            }
        }

        if (i >= 0)
            RELEASE_AND_RETURN(scope, baseValue.get(globalObject, static_cast<uint32_t>(i)));
    }

    baseValue.requireObjectCoercible(globalObject);
    RETURN_IF_EXCEPTION(scope, JSValue());
    auto property = subscript.toPropertyKey(globalObject);
    RETURN_IF_EXCEPTION(scope, JSValue());

    ASSERT(callFrame->bytecodeIndex() != BytecodeIndex(0));
    RELEASE_AND_RETURN(scope, baseValue.get(globalObject, property));
}

extern "C" {

EncodedJSValue JIT_OPERATION operationGetByValGeneric(JSGlobalObject* globalObject, StructureStubInfo* stubInfo, ArrayProfile* profile, EncodedJSValue encodedBase, EncodedJSValue encodedSubscript)
{
    VM& vm = globalObject->vm();
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);
    JSValue baseValue = JSValue::decode(encodedBase);
    JSValue subscript = JSValue::decode(encodedSubscript);

    stubInfo->tookSlowPath = true;

    return JSValue::encode(getByVal(globalObject, callFrame, profile, baseValue, subscript));
}

EncodedJSValue JIT_OPERATION operationGetByValOptimize(JSGlobalObject* globalObject, StructureStubInfo* stubInfo, ArrayProfile* profile, EncodedJSValue encodedBase, EncodedJSValue encodedSubscript)
{
    VM& vm = globalObject->vm();
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);
    auto scope = DECLARE_THROW_SCOPE(vm);

    JSValue baseValue = JSValue::decode(encodedBase);
    JSValue subscript = JSValue::decode(encodedSubscript);

    CodeBlock* codeBlock = callFrame->codeBlock();

    if (baseValue.isCell() && subscript.isInt32()) {
        Structure* structure = baseValue.asCell()->structure(vm);
        if (stubInfo->considerCaching(vm, codeBlock, structure)) {
            if (profile) {
                ConcurrentJSLocker locker(codeBlock->m_lock);
                profile->computeUpdatedPrediction(locker, codeBlock, structure);
            }
            repatchArrayGetByVal(globalObject, codeBlock, baseValue, subscript, *stubInfo);
        }
    }

    if (baseValue.isCell() && isStringOrSymbol(subscript)) {
        const Identifier propertyName = subscript.toPropertyKey(globalObject);
        RETURN_IF_EXCEPTION(scope, encodedJSValue());
        if (subscript.isSymbol() || !parseIndex(propertyName)) {
            scope.release();
            return JSValue::encode(baseValue.getPropertySlot(globalObject, propertyName, [&] (bool found, PropertySlot& slot) -> JSValue {
                LOG_IC((ICEvent::OperationGetByValOptimize, baseValue.classInfoOrNull(vm), propertyName, baseValue == slot.slotBase())); 
                
                if (stubInfo->considerCaching(vm, codeBlock, baseValue.structureOrNull(), propertyName.impl()))
                    repatchGetBy(globalObject, codeBlock, baseValue, propertyName, slot, *stubInfo, GetByKind::NormalByVal);
                return found ? slot.getValue(globalObject, propertyName) : jsUndefined();
            }));
        }
    }

    RELEASE_AND_RETURN(scope, JSValue::encode(getByVal(globalObject, callFrame, profile, baseValue, subscript)));
}

EncodedJSValue JIT_OPERATION operationGetByVal(JSGlobalObject* globalObject, EncodedJSValue encodedBase, EncodedJSValue encodedProperty)
{
    VM& vm = globalObject->vm();
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);
    auto scope = DECLARE_THROW_SCOPE(vm);

    JSValue baseValue = JSValue::decode(encodedBase);
    JSValue property = JSValue::decode(encodedProperty);

    if (LIKELY(baseValue.isCell())) {
        JSCell* base = baseValue.asCell();

        if (property.isUInt32())
            RELEASE_AND_RETURN(scope, getByValWithIndex(globalObject, base, property.asUInt32()));

        if (property.isDouble()) {
            double propertyAsDouble = property.asDouble();
            uint32_t propertyAsUInt32 = static_cast<uint32_t>(propertyAsDouble);
            if (propertyAsUInt32 == propertyAsDouble && isIndex(propertyAsUInt32))
                RELEASE_AND_RETURN(scope, getByValWithIndex(globalObject, base, propertyAsUInt32));
        } else if (property.isString()) {
            Structure& structure = *base->structure(vm);
            if (JSCell::canUseFastGetOwnProperty(structure)) {
                RefPtr<AtomStringImpl> existingAtomString = asString(property)->toExistingAtomString(globalObject);
                RETURN_IF_EXCEPTION(scope, encodedJSValue());
                if (existingAtomString) {
                    if (JSValue result = base->fastGetOwnProperty(vm, structure, existingAtomString.get()))
                        return JSValue::encode(result);
                }
            }
        }
    }

    baseValue.requireObjectCoercible(globalObject);
    RETURN_IF_EXCEPTION(scope, encodedJSValue());
    auto propertyName = property.toPropertyKey(globalObject);
    RETURN_IF_EXCEPTION(scope, encodedJSValue());
    RELEASE_AND_RETURN(scope, JSValue::encode(baseValue.get(globalObject, propertyName)));
}


EncodedJSValue JIT_OPERATION operationHasIndexedPropertyDefault(JSGlobalObject* globalObject, EncodedJSValue encodedBase, EncodedJSValue encodedSubscript, ByValInfo* byValInfo)
{
    VM& vm = globalObject->vm();
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);
    JSValue baseValue = JSValue::decode(encodedBase);
    JSValue subscript = JSValue::decode(encodedSubscript);
    
    ASSERT(baseValue.isObject());
    ASSERT(subscript.isUInt32AsAnyInt());

    JSObject* object = asObject(baseValue);
    bool didOptimize = false;

    ASSERT(callFrame->bytecodeIndex() != BytecodeIndex(0));
    ASSERT(!byValInfo->stubRoutine);
    
    if (hasOptimizableIndexing(object->structure(vm))) {
        // Attempt to optimize.
        JITArrayMode arrayMode = jitArrayModeForStructure(object->structure(vm));
        if (arrayMode != byValInfo->arrayMode) {
            JIT::compileHasIndexedProperty(vm, callFrame->codeBlock(), byValInfo, ReturnAddressPtr(OUR_RETURN_ADDRESS), arrayMode);
            didOptimize = true;
        }
    }
    
    if (!didOptimize) {
        // If we take slow path more than 10 times without patching then make sure we
        // never make that mistake again. Or, if we failed to patch and we have some object
        // that intercepts indexed get, then don't even wait until 10 times. For cases
        // where we see non-index-intercepting objects, this gives 10 iterations worth of
        // opportunity for us to observe that the get_by_val may be polymorphic.
        if (++byValInfo->slowPathCount >= 10
            || object->structure(vm)->typeInfo().interceptsGetOwnPropertySlotByIndexEvenWhenLengthIsNotZero()) {
            // Don't ever try to optimize.
            ctiPatchCallByReturnAddress(ReturnAddressPtr(OUR_RETURN_ADDRESS), operationHasIndexedPropertyGeneric);
        }
    }

    uint32_t index = subscript.asUInt32AsAnyInt();
    if (object->canGetIndexQuickly(index))
        return JSValue::encode(JSValue(JSValue::JSTrue));

    if (!CommonSlowPaths::canAccessArgumentIndexQuickly(*object, index))
        byValInfo->arrayProfile->setOutOfBounds();
    return JSValue::encode(jsBoolean(object->hasPropertyGeneric(globalObject, index, PropertySlot::InternalMethodType::GetOwnProperty)));
}
    
EncodedJSValue JIT_OPERATION operationHasIndexedPropertyGeneric(JSGlobalObject* globalObject, EncodedJSValue encodedBase, EncodedJSValue encodedSubscript, ByValInfo* byValInfo)
{
    VM& vm = globalObject->vm();
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);
    JSValue baseValue = JSValue::decode(encodedBase);
    JSValue subscript = JSValue::decode(encodedSubscript);
    
    ASSERT(baseValue.isObject());
    ASSERT(subscript.isUInt32AsAnyInt());

    JSObject* object = asObject(baseValue);
    uint32_t index = subscript.asUInt32AsAnyInt();
    if (object->canGetIndexQuickly(index))
        return JSValue::encode(JSValue(JSValue::JSTrue));

    if (!CommonSlowPaths::canAccessArgumentIndexQuickly(*object, index))
        byValInfo->arrayProfile->setOutOfBounds();
    return JSValue::encode(jsBoolean(object->hasPropertyGeneric(globalObject, index, PropertySlot::InternalMethodType::GetOwnProperty)));
}
    
static bool deleteById(JSGlobalObject* globalObject, CallFrame* callFrame, VM& vm, JSValue base, UniquedStringImpl* uid)
{
    auto scope = DECLARE_THROW_SCOPE(vm);

    JSObject* baseObj = base.toObject(globalObject);
    RETURN_IF_EXCEPTION(scope, false);
    if (!baseObj)
        return false;
    bool couldDelete = baseObj->methodTable(vm)->deleteProperty(baseObj, globalObject, Identifier::fromUid(vm, uid));
    RETURN_IF_EXCEPTION(scope, false);
    if (!couldDelete && callFrame->codeBlock()->isStrictMode())
        throwTypeError(globalObject, scope, UnableToDeletePropertyError);
    return couldDelete;
}


EncodedJSValue JIT_OPERATION operationDeleteByIdJSResult(JSGlobalObject* globalObject, EncodedJSValue encodedBase, UniquedStringImpl* uid)
{
    VM& vm = globalObject->vm();
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);
    return JSValue::encode(jsBoolean(deleteById(globalObject, callFrame, vm, JSValue::decode(encodedBase), uid)));
}

size_t JIT_OPERATION operationDeleteById(JSGlobalObject* globalObject, EncodedJSValue encodedBase, UniquedStringImpl* uid)
{
    VM& vm = globalObject->vm();
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);
    return deleteById(globalObject, callFrame, vm, JSValue::decode(encodedBase), uid);
}

static bool deleteByVal(JSGlobalObject* globalObject, CallFrame* callFrame, VM& vm, JSValue base, JSValue key)
{
    auto scope = DECLARE_THROW_SCOPE(vm);

    JSObject* baseObj = base.toObject(globalObject);
    RETURN_IF_EXCEPTION(scope, false);
    if (!baseObj)
        return false;

    bool couldDelete;
    uint32_t index;
    if (key.getUInt32(index))
        couldDelete = baseObj->methodTable(vm)->deletePropertyByIndex(baseObj, globalObject, index);
    else {
        Identifier property = key.toPropertyKey(globalObject);
        RETURN_IF_EXCEPTION(scope, false);
        couldDelete = baseObj->methodTable(vm)->deleteProperty(baseObj, globalObject, property);
    }
    RETURN_IF_EXCEPTION(scope, false);
    if (!couldDelete && callFrame->codeBlock()->isStrictMode())
        throwTypeError(globalObject, scope, UnableToDeletePropertyError);
    return couldDelete;
}

EncodedJSValue JIT_OPERATION operationDeleteByValJSResult(JSGlobalObject* globalObject, EncodedJSValue encodedBase,  EncodedJSValue encodedKey)
{
    VM& vm = globalObject->vm();
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);
    return JSValue::encode(jsBoolean(deleteByVal(globalObject, callFrame, vm, JSValue::decode(encodedBase), JSValue::decode(encodedKey))));
}

size_t JIT_OPERATION operationDeleteByVal(JSGlobalObject* globalObject, EncodedJSValue encodedBase, EncodedJSValue encodedKey)
{
    VM& vm = globalObject->vm();
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);
    return deleteByVal(globalObject, callFrame, vm, JSValue::decode(encodedBase), JSValue::decode(encodedKey));
}

JSCell* JIT_OPERATION operationPushWithScope(JSGlobalObject* globalObject, JSCell* currentScopeCell, EncodedJSValue objectValue)
{
    VM& vm = globalObject->vm();
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);
    auto scope = DECLARE_THROW_SCOPE(vm);

    JSObject* object = JSValue::decode(objectValue).toObject(globalObject);
    RETURN_IF_EXCEPTION(scope, nullptr);

    JSScope* currentScope = jsCast<JSScope*>(currentScopeCell);

    return JSWithScope::create(vm, globalObject, currentScope, object);
}

JSCell* JIT_OPERATION operationPushWithScopeObject(JSGlobalObject* globalObject, JSCell* currentScopeCell, JSObject* object)
{
    VM& vm = globalObject->vm();
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);
    JSScope* currentScope = jsCast<JSScope*>(currentScopeCell);
    return JSWithScope::create(vm, globalObject, currentScope, object);
}

EncodedJSValue JIT_OPERATION operationInstanceOf(JSGlobalObject* globalObject, EncodedJSValue encodedValue, EncodedJSValue encodedProto)
{
    VM& vm = globalObject->vm();
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);
    JSValue value = JSValue::decode(encodedValue);
    JSValue proto = JSValue::decode(encodedProto);
    
    bool result = JSObject::defaultHasInstance(globalObject, value, proto);
    return JSValue::encode(jsBoolean(result));
}

EncodedJSValue JIT_OPERATION operationInstanceOfGeneric(JSGlobalObject* globalObject, StructureStubInfo* stubInfo, EncodedJSValue encodedValue, EncodedJSValue encodedProto)
{
    VM& vm = globalObject->vm();
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);
    JSValue value = JSValue::decode(encodedValue);
    JSValue proto = JSValue::decode(encodedProto);
    
    stubInfo->tookSlowPath = true;
    
    bool result = JSObject::defaultHasInstance(globalObject, value, proto);
    return JSValue::encode(jsBoolean(result));
}

EncodedJSValue JIT_OPERATION operationInstanceOfOptimize(JSGlobalObject* globalObject, StructureStubInfo* stubInfo, EncodedJSValue encodedValue, EncodedJSValue encodedProto)
{
    VM& vm = globalObject->vm();
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue value = JSValue::decode(encodedValue);
    JSValue proto = JSValue::decode(encodedProto);
    
    bool result = JSObject::defaultHasInstance(globalObject, value, proto);
    RETURN_IF_EXCEPTION(scope, JSValue::encode(jsUndefined()));
    
    CodeBlock* codeBlock = callFrame->codeBlock();
    if (stubInfo->considerCaching(vm, codeBlock, value.structureOrNull()))
        repatchInstanceOf(globalObject, codeBlock, value, proto, *stubInfo, result);
    
    return JSValue::encode(jsBoolean(result));
}

int32_t JIT_OPERATION operationSizeFrameForForwardArguments(JSGlobalObject* globalObject, EncodedJSValue, int32_t numUsedStackSlots, int32_t)
{
    VM& vm = globalObject->vm();
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);
    return sizeFrameForForwardArguments(globalObject, callFrame, vm, numUsedStackSlots);
}

int32_t JIT_OPERATION operationSizeFrameForVarargs(JSGlobalObject* globalObject, EncodedJSValue encodedArguments, int32_t numUsedStackSlots, int32_t firstVarArgOffset)
{
    VM& vm = globalObject->vm();
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);
    JSValue arguments = JSValue::decode(encodedArguments);
    return sizeFrameForVarargs(globalObject, callFrame, vm, arguments, numUsedStackSlots, firstVarArgOffset);
}

CallFrame* JIT_OPERATION operationSetupForwardArgumentsFrame(JSGlobalObject* globalObject, CallFrame* newCallFrame, EncodedJSValue, int32_t, int32_t length)
{
    VM& vm = globalObject->vm();
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);
    setupForwardArgumentsFrame(globalObject, callFrame, newCallFrame, length);
    return newCallFrame;
}

CallFrame* JIT_OPERATION operationSetupVarargsFrame(JSGlobalObject* globalObject, CallFrame* newCallFrame, EncodedJSValue encodedArguments, int32_t firstVarArgOffset, int32_t length)
{
    VM& vm = globalObject->vm();
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);
    JSValue arguments = JSValue::decode(encodedArguments);
    setupVarargsFrame(globalObject, callFrame, newCallFrame, arguments, firstVarArgOffset, length);
    return newCallFrame;
}

char* JIT_OPERATION operationSwitchCharWithUnknownKeyType(JSGlobalObject* globalObject, EncodedJSValue encodedKey, size_t tableIndex)
{
    VM& vm = globalObject->vm();
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);
    auto throwScope = DECLARE_THROW_SCOPE(vm);

    JSValue key = JSValue::decode(encodedKey);
    CodeBlock* codeBlock = callFrame->codeBlock();

    SimpleJumpTable& jumpTable = codeBlock->switchJumpTable(tableIndex);
    void* result = jumpTable.ctiDefault.executableAddress();

    if (key.isString()) {
        JSString* string = asString(key);
        if (string->length() == 1) {
            String value = string->value(globalObject);
            RETURN_IF_EXCEPTION(throwScope, nullptr);
            result = jumpTable.ctiForValue(value[0]).executableAddress();
        }
    }

    assertIsTaggedWith(result, JSSwitchPtrTag);
    return reinterpret_cast<char*>(result);
}

char* JIT_OPERATION operationSwitchImmWithUnknownKeyType(VM* vmPointer, EncodedJSValue encodedKey, size_t tableIndex)
{
    VM& vm = *vmPointer;
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);
    JSValue key = JSValue::decode(encodedKey);
    CodeBlock* codeBlock = callFrame->codeBlock();

    SimpleJumpTable& jumpTable = codeBlock->switchJumpTable(tableIndex);
    void* result;
    if (key.isInt32())
        result = jumpTable.ctiForValue(key.asInt32()).executableAddress();
    else if (key.isDouble() && key.asDouble() == static_cast<int32_t>(key.asDouble()))
        result = jumpTable.ctiForValue(static_cast<int32_t>(key.asDouble())).executableAddress();
    else
        result = jumpTable.ctiDefault.executableAddress();
    assertIsTaggedWith(result, JSSwitchPtrTag);
    return reinterpret_cast<char*>(result);
}

char* JIT_OPERATION operationSwitchStringWithUnknownKeyType(JSGlobalObject* globalObject, EncodedJSValue encodedKey, size_t tableIndex)
{
    VM& vm = globalObject->vm();
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);
    JSValue key = JSValue::decode(encodedKey);
    CodeBlock* codeBlock = callFrame->codeBlock();
    auto throwScope = DECLARE_THROW_SCOPE(vm);

    void* result;
    StringJumpTable& jumpTable = codeBlock->stringSwitchJumpTable(tableIndex);

    if (key.isString()) {
        StringImpl* value = asString(key)->value(globalObject).impl();

        RETURN_IF_EXCEPTION(throwScope, nullptr);

        result = jumpTable.ctiForValue(value).executableAddress();
    } else
        result = jumpTable.ctiDefault.executableAddress();

    assertIsTaggedWith(result, JSSwitchPtrTag);
    return reinterpret_cast<char*>(result);
}

EncodedJSValue JIT_OPERATION operationGetFromScope(JSGlobalObject* globalObject, const Instruction* pc)
{
    VM& vm = globalObject->vm();
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);
    auto throwScope = DECLARE_THROW_SCOPE(vm);

    CodeBlock* codeBlock = callFrame->codeBlock();

    auto bytecode = pc->as<OpGetFromScope>();
    const Identifier& ident = codeBlock->identifier(bytecode.m_var);
    JSObject* scope = jsCast<JSObject*>(callFrame->uncheckedR(bytecode.m_scope.offset()).jsValue());
    GetPutInfo& getPutInfo = bytecode.metadata(codeBlock).m_getPutInfo;

    // ModuleVar is always converted to ClosureVar for get_from_scope.
    ASSERT(getPutInfo.resolveType() != ModuleVar);

    RELEASE_AND_RETURN(throwScope, JSValue::encode(scope->getPropertySlot(globalObject, ident, [&] (bool found, PropertySlot& slot) -> JSValue {
        if (!found) {
            if (getPutInfo.resolveMode() == ThrowIfNotFound)
                throwException(globalObject, throwScope, createUndefinedVariableError(globalObject, ident));
            return jsUndefined();
        }

        JSValue result = JSValue();
        if (scope->isGlobalLexicalEnvironment()) {
            // When we can't statically prove we need a TDZ check, we must perform the check on the slow path.
            result = slot.getValue(globalObject, ident);
            if (result == jsTDZValue()) {
                throwException(globalObject, throwScope, createTDZError(globalObject));
                return jsUndefined();
            }
        }

        CommonSlowPaths::tryCacheGetFromScopeGlobal(globalObject, codeBlock, vm, bytecode, scope, slot, ident);

        if (!result)
            return slot.getValue(globalObject, ident);
        return result;
    })));
}

void JIT_OPERATION operationPutToScope(JSGlobalObject* globalObject, const Instruction* pc)
{
    VM& vm = globalObject->vm();
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);
    auto throwScope = DECLARE_THROW_SCOPE(vm);

    CodeBlock* codeBlock = callFrame->codeBlock();
    auto bytecode = pc->as<OpPutToScope>();
    auto& metadata = bytecode.metadata(codeBlock);

    const Identifier& ident = codeBlock->identifier(bytecode.m_var);
    JSObject* scope = jsCast<JSObject*>(callFrame->uncheckedR(bytecode.m_scope.offset()).jsValue());
    JSValue value = callFrame->r(bytecode.m_value.offset()).jsValue();
    GetPutInfo& getPutInfo = metadata.m_getPutInfo;

    // ModuleVar does not keep the scope register value alive in DFG.
    ASSERT(getPutInfo.resolveType() != ModuleVar);

    if (getPutInfo.resolveType() == LocalClosureVar) {
        JSLexicalEnvironment* environment = jsCast<JSLexicalEnvironment*>(scope);
        environment->variableAt(ScopeOffset(metadata.m_operand)).set(vm, environment, value);
        if (WatchpointSet* set = metadata.m_watchpointSet)
            set->touch(vm, "Executed op_put_scope<LocalClosureVar>");
        return;
    }

    bool hasProperty = scope->hasProperty(globalObject, ident);
    RETURN_IF_EXCEPTION(throwScope, void());
    if (hasProperty
        && scope->isGlobalLexicalEnvironment()
        && !isInitialization(getPutInfo.initializationMode())) {
        // When we can't statically prove we need a TDZ check, we must perform the check on the slow path.
        PropertySlot slot(scope, PropertySlot::InternalMethodType::Get);
        JSGlobalLexicalEnvironment::getOwnPropertySlot(scope, globalObject, ident, slot);
        if (slot.getValue(globalObject, ident) == jsTDZValue()) {
            throwException(globalObject, throwScope, createTDZError(globalObject));
            return;
        }
    }

    if (getPutInfo.resolveMode() == ThrowIfNotFound && !hasProperty) {
        throwException(globalObject, throwScope, createUndefinedVariableError(globalObject, ident));
        return;
    }

    PutPropertySlot slot(scope, codeBlock->isStrictMode(), PutPropertySlot::UnknownContext, isInitialization(getPutInfo.initializationMode()));
    scope->methodTable(vm)->put(scope, globalObject, ident, value, slot);
    
    RETURN_IF_EXCEPTION(throwScope, void());

    CommonSlowPaths::tryCachePutToScopeGlobal(globalObject, codeBlock, bytecode, scope, slot, ident);
}

void JIT_OPERATION operationThrow(JSGlobalObject* globalObject, EncodedJSValue encodedExceptionValue)
{
    VM& vm = globalObject->vm();
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);
    auto scope = DECLARE_THROW_SCOPE(vm);

    JSValue exceptionValue = JSValue::decode(encodedExceptionValue);
    throwException(globalObject, scope, exceptionValue);

    // Results stored out-of-band in vm.targetMachinePCForThrow & vm.callFrameForCatch
    genericUnwind(vm, callFrame);
}

char* JIT_OPERATION operationReallocateButterflyToHavePropertyStorageWithInitialCapacity(VM* vmPointer, JSObject* object)
{
    VM& vm = *vmPointer;
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);

    ASSERT(!object->structure(vm)->outOfLineCapacity());
    Butterfly* result = object->allocateMoreOutOfLineStorage(vm, 0, initialOutOfLineCapacity);
    object->nukeStructureAndSetButterfly(vm, object->structureID(), result);
    return reinterpret_cast<char*>(result);
}

char* JIT_OPERATION operationReallocateButterflyToGrowPropertyStorage(VM* vmPointer, JSObject* object, size_t newSize)
{
    VM& vm = *vmPointer;
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);

    Butterfly* result = object->allocateMoreOutOfLineStorage(vm, object->structure(vm)->outOfLineCapacity(), newSize);
    object->nukeStructureAndSetButterfly(vm, object->structureID(), result);
    return reinterpret_cast<char*>(result);
}

void JIT_OPERATION operationOSRWriteBarrier(VM* vmPointer, JSCell* cell)
{
    VM& vm = *vmPointer;
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);
    vm.heap.writeBarrier(cell);
}

void JIT_OPERATION operationWriteBarrierSlowPath(VM* vmPointer, JSCell* cell)
{
    VM& vm = *vmPointer;
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);
    vm.heap.writeBarrierSlowPath(cell);
}

void JIT_OPERATION operationLookupExceptionHandler(VM* vmPointer)
{
    VM& vm = *vmPointer;
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);
    genericUnwind(vm, callFrame);
    ASSERT(vm.targetMachinePCForThrow);
}

void JIT_OPERATION operationLookupExceptionHandlerFromCallerFrame(VM* vmPointer)
{
    VM& vm = *vmPointer;
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);
    ASSERT(callFrame->isStackOverflowFrame());
    ASSERT(jsCast<ErrorInstance*>(vm.exceptionForInspection()->value().asCell())->isStackOverflowError());
    genericUnwind(vm, callFrame);
    ASSERT(vm.targetMachinePCForThrow);
}

void JIT_OPERATION operationVMHandleException(VM* vmPointer)
{
    VM& vm = *vmPointer;
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);
    genericUnwind(vm, callFrame);
}

// This function "should" just take the JSGlobalObject*, but doing so would make it more difficult
// to call from exception check sites. So, unlike all of our other functions, we allow
// ourselves to play some gnarly ABI tricks just to simplify the calling convention. This is
// particularly safe here since this is never called on the critical path - it's only for
// testing.
void JIT_OPERATION operationExceptionFuzz(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);
    auto scope = DECLARE_THROW_SCOPE(vm);
    UNUSED_PARAM(scope);
#if COMPILER(GCC_COMPATIBLE)
    void* returnPC = __builtin_return_address(0);
    doExceptionFuzzing(globalObject, scope, "JITOperations", returnPC);
#endif // COMPILER(GCC_COMPATIBLE)
}

EncodedJSValue JIT_OPERATION operationValueAdd(JSGlobalObject* globalObject, EncodedJSValue encodedOp1, EncodedJSValue encodedOp2)
{
    VM& vm = globalObject->vm();
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);
    return JSValue::encode(jsAdd(globalObject, JSValue::decode(encodedOp1), JSValue::decode(encodedOp2)));
}

EncodedJSValue JIT_OPERATION operationValueAddProfiled(JSGlobalObject* globalObject, EncodedJSValue encodedOp1, EncodedJSValue encodedOp2, BinaryArithProfile* arithProfile)
{
    ASSERT(arithProfile);
    VM& vm = globalObject->vm();
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);
    return JSValue::encode(profiledAdd(globalObject, JSValue::decode(encodedOp1), JSValue::decode(encodedOp2), *arithProfile));
}

EncodedJSValue JIT_OPERATION operationValueAddProfiledOptimize(JSGlobalObject* globalObject, EncodedJSValue encodedOp1, EncodedJSValue encodedOp2, JITAddIC* addIC)
{
    VM& vm = globalObject->vm();
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);
    
    JSValue op1 = JSValue::decode(encodedOp1);
    JSValue op2 = JSValue::decode(encodedOp2);

    BinaryArithProfile* arithProfile = addIC->arithProfile();
    ASSERT(arithProfile);
    arithProfile->observeLHSAndRHS(op1, op2);
    auto nonOptimizeVariant = operationValueAddProfiledNoOptimize;
    addIC->generateOutOfLine(callFrame->codeBlock(), nonOptimizeVariant);

#if ENABLE(MATH_IC_STATS)
    callFrame->codeBlock()->dumpMathICStats();
#endif
    
    JSValue result = jsAdd(globalObject, op1, op2);
    arithProfile->observeResult(result);

    return JSValue::encode(result);
}

EncodedJSValue JIT_OPERATION operationValueAddProfiledNoOptimize(JSGlobalObject* globalObject, EncodedJSValue encodedOp1, EncodedJSValue encodedOp2, JITAddIC* addIC)
{
    VM& vm = globalObject->vm();
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);

    BinaryArithProfile* arithProfile = addIC->arithProfile();
    ASSERT(arithProfile);
    return JSValue::encode(profiledAdd(globalObject, JSValue::decode(encodedOp1), JSValue::decode(encodedOp2), *arithProfile));
}

EncodedJSValue JIT_OPERATION operationValueAddOptimize(JSGlobalObject* globalObject, EncodedJSValue encodedOp1, EncodedJSValue encodedOp2, JITAddIC* addIC)
{
    VM& vm = globalObject->vm();
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);

    JSValue op1 = JSValue::decode(encodedOp1);
    JSValue op2 = JSValue::decode(encodedOp2);

    auto nonOptimizeVariant = operationValueAddNoOptimize;
    if (BinaryArithProfile* arithProfile = addIC->arithProfile())
        arithProfile->observeLHSAndRHS(op1, op2);
    addIC->generateOutOfLine(callFrame->codeBlock(), nonOptimizeVariant);

#if ENABLE(MATH_IC_STATS)
    callFrame->codeBlock()->dumpMathICStats();
#endif

    return JSValue::encode(jsAdd(globalObject, op1, op2));
}

EncodedJSValue JIT_OPERATION operationValueAddNoOptimize(JSGlobalObject* globalObject, EncodedJSValue encodedOp1, EncodedJSValue encodedOp2, JITAddIC*)
{
    VM& vm = globalObject->vm();
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);
    
    JSValue op1 = JSValue::decode(encodedOp1);
    JSValue op2 = JSValue::decode(encodedOp2);
    
    JSValue result = jsAdd(globalObject, op1, op2);

    return JSValue::encode(result);
}

ALWAYS_INLINE static EncodedJSValue unprofiledMul(JSGlobalObject* globalObject, EncodedJSValue encodedOp1, EncodedJSValue encodedOp2)
{
    JSValue op1 = JSValue::decode(encodedOp1);
    JSValue op2 = JSValue::decode(encodedOp2);

    return JSValue::encode(jsMul(globalObject, op1, op2));
}

ALWAYS_INLINE static EncodedJSValue profiledMul(JSGlobalObject* globalObject, EncodedJSValue encodedOp1, EncodedJSValue encodedOp2, BinaryArithProfile& arithProfile, bool shouldObserveLHSAndRHSTypes = true)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue op1 = JSValue::decode(encodedOp1);
    JSValue op2 = JSValue::decode(encodedOp2);

    if (shouldObserveLHSAndRHSTypes)
        arithProfile.observeLHSAndRHS(op1, op2);

    JSValue result = jsMul(globalObject, op1, op2);
    RETURN_IF_EXCEPTION(scope, encodedJSValue());
    arithProfile.observeResult(result);
    return JSValue::encode(result);
}

EncodedJSValue JIT_OPERATION operationValueMul(JSGlobalObject* globalObject, EncodedJSValue encodedOp1, EncodedJSValue encodedOp2)
{
    VM& vm = globalObject->vm();
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);

    return unprofiledMul(globalObject, encodedOp1, encodedOp2);
}

EncodedJSValue JIT_OPERATION operationValueMulNoOptimize(JSGlobalObject* globalObject, EncodedJSValue encodedOp1, EncodedJSValue encodedOp2, JITMulIC*)
{
    VM& vm = globalObject->vm();
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);

    return unprofiledMul(globalObject, encodedOp1, encodedOp2);
}

EncodedJSValue JIT_OPERATION operationValueMulOptimize(JSGlobalObject* globalObject, EncodedJSValue encodedOp1, EncodedJSValue encodedOp2, JITMulIC* mulIC)
{
    VM& vm = globalObject->vm();
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);

    auto nonOptimizeVariant = operationValueMulNoOptimize;
    if (BinaryArithProfile* arithProfile = mulIC->arithProfile())
        arithProfile->observeLHSAndRHS(JSValue::decode(encodedOp1), JSValue::decode(encodedOp2));
    mulIC->generateOutOfLine(callFrame->codeBlock(), nonOptimizeVariant);

#if ENABLE(MATH_IC_STATS)
    callFrame->codeBlock()->dumpMathICStats();
#endif

    return unprofiledMul(globalObject, encodedOp1, encodedOp2);
}

EncodedJSValue JIT_OPERATION operationValueMulProfiled(JSGlobalObject* globalObject, EncodedJSValue encodedOp1, EncodedJSValue encodedOp2, BinaryArithProfile* arithProfile)
{
    VM& vm = globalObject->vm();
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);

    ASSERT(arithProfile);
    return profiledMul(globalObject, encodedOp1, encodedOp2, *arithProfile);
}

EncodedJSValue JIT_OPERATION operationValueMulProfiledOptimize(JSGlobalObject* globalObject, EncodedJSValue encodedOp1, EncodedJSValue encodedOp2, JITMulIC* mulIC)
{
    VM& vm = globalObject->vm();
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);

    BinaryArithProfile* arithProfile = mulIC->arithProfile();
    ASSERT(arithProfile);
    arithProfile->observeLHSAndRHS(JSValue::decode(encodedOp1), JSValue::decode(encodedOp2));
    auto nonOptimizeVariant = operationValueMulProfiledNoOptimize;
    mulIC->generateOutOfLine(callFrame->codeBlock(), nonOptimizeVariant);

#if ENABLE(MATH_IC_STATS)
    callFrame->codeBlock()->dumpMathICStats();
#endif

    return profiledMul(globalObject, encodedOp1, encodedOp2, *arithProfile, false);
}

EncodedJSValue JIT_OPERATION operationValueMulProfiledNoOptimize(JSGlobalObject* globalObject, EncodedJSValue encodedOp1, EncodedJSValue encodedOp2, JITMulIC* mulIC)
{
    VM& vm = globalObject->vm();
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);

    BinaryArithProfile* arithProfile = mulIC->arithProfile();
    ASSERT(arithProfile);
    return profiledMul(globalObject, encodedOp1, encodedOp2, *arithProfile);
}

EncodedJSValue JIT_OPERATION operationArithNegate(JSGlobalObject* globalObject, EncodedJSValue encodedOperand)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);

    JSValue operand = JSValue::decode(encodedOperand);

    JSValue primValue = operand.toPrimitive(globalObject, PreferNumber);
    RETURN_IF_EXCEPTION(scope, encodedJSValue());

    if (primValue.isBigInt())
        return JSValue::encode(JSBigInt::unaryMinus(vm, asBigInt(primValue)));

    double number = primValue.toNumber(globalObject);
    RETURN_IF_EXCEPTION(scope, encodedJSValue());
    return JSValue::encode(jsNumber(-number));

}

EncodedJSValue JIT_OPERATION operationArithNegateProfiled(JSGlobalObject* globalObject, EncodedJSValue encodedOperand, UnaryArithProfile* arithProfile)
{
    ASSERT(arithProfile);
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);

    JSValue operand = JSValue::decode(encodedOperand);
    arithProfile->observeArg(operand);

    JSValue primValue = operand.toPrimitive(globalObject);
    RETURN_IF_EXCEPTION(scope, encodedJSValue());

    if (primValue.isBigInt()) {
        JSBigInt* result = JSBigInt::unaryMinus(vm, asBigInt(primValue));
        arithProfile->observeResult(result);

        return JSValue::encode(result);
    }

    double number = primValue.toNumber(globalObject);
    RETURN_IF_EXCEPTION(scope, encodedJSValue());
    JSValue result = jsNumber(-number);
    arithProfile->observeResult(result);
    return JSValue::encode(result);
}

EncodedJSValue JIT_OPERATION operationArithNegateProfiledOptimize(JSGlobalObject* globalObject, EncodedJSValue encodedOperand, JITNegIC* negIC)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);
    
    JSValue operand = JSValue::decode(encodedOperand);

    UnaryArithProfile* arithProfile = negIC->arithProfile();
    ASSERT(arithProfile);
    arithProfile->observeArg(operand);
    negIC->generateOutOfLine(callFrame->codeBlock(), operationArithNegateProfiled);

#if ENABLE(MATH_IC_STATS)
    callFrame->codeBlock()->dumpMathICStats();
#endif
    
    JSValue primValue = operand.toPrimitive(globalObject);
    RETURN_IF_EXCEPTION(scope, encodedJSValue());
    
    if (primValue.isBigInt()) {
        JSBigInt* result = JSBigInt::unaryMinus(vm, asBigInt(primValue));
        arithProfile->observeResult(result);
        return JSValue::encode(result);
    }

    double number = primValue.toNumber(globalObject);
    RETURN_IF_EXCEPTION(scope, encodedJSValue());
    JSValue result = jsNumber(-number);
    arithProfile->observeResult(result);
    return JSValue::encode(result);
}

EncodedJSValue JIT_OPERATION operationArithNegateOptimize(JSGlobalObject* globalObject, EncodedJSValue encodedOperand, JITNegIC* negIC)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);

    JSValue operand = JSValue::decode(encodedOperand);

    if (UnaryArithProfile* arithProfile = negIC->arithProfile())
        arithProfile->observeArg(operand);
    negIC->generateOutOfLine(callFrame->codeBlock(), operationArithNegate);

#if ENABLE(MATH_IC_STATS)
    callFrame->codeBlock()->dumpMathICStats();
#endif

    JSValue primValue = operand.toPrimitive(globalObject);
    RETURN_IF_EXCEPTION(scope, encodedJSValue());
    
    if (primValue.isBigInt())
        return JSValue::encode(JSBigInt::unaryMinus(vm, asBigInt(primValue)));

    double number = primValue.toNumber(globalObject);
    RETURN_IF_EXCEPTION(scope, encodedJSValue());
    return JSValue::encode(jsNumber(-number));
}

ALWAYS_INLINE static EncodedJSValue unprofiledSub(JSGlobalObject* globalObject, EncodedJSValue encodedOp1, EncodedJSValue encodedOp2)
{
    JSValue op1 = JSValue::decode(encodedOp1);
    JSValue op2 = JSValue::decode(encodedOp2);
    
    return JSValue::encode(jsSub(globalObject, op1, op2));
}

ALWAYS_INLINE static EncodedJSValue profiledSub(VM& vm, JSGlobalObject* globalObject, EncodedJSValue encodedOp1, EncodedJSValue encodedOp2, BinaryArithProfile& arithProfile, bool shouldObserveLHSAndRHSTypes = true)
{
    auto scope = DECLARE_THROW_SCOPE(vm);

    JSValue op1 = JSValue::decode(encodedOp1);
    JSValue op2 = JSValue::decode(encodedOp2);

    if (shouldObserveLHSAndRHSTypes)
        arithProfile.observeLHSAndRHS(op1, op2);

    JSValue result = jsSub(globalObject, op1, op2);
    RETURN_IF_EXCEPTION(scope, encodedJSValue());
    arithProfile.observeResult(result);
    return JSValue::encode(result);
}

EncodedJSValue JIT_OPERATION operationValueSub(JSGlobalObject* globalObject, EncodedJSValue encodedOp1, EncodedJSValue encodedOp2)
{
    VM& vm = globalObject->vm();
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);
    return unprofiledSub(globalObject, encodedOp1, encodedOp2);
}

EncodedJSValue JIT_OPERATION operationValueSubProfiled(JSGlobalObject* globalObject, EncodedJSValue encodedOp1, EncodedJSValue encodedOp2, BinaryArithProfile* arithProfile)
{
    ASSERT(arithProfile);

    VM& vm = globalObject->vm();
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);

    return profiledSub(vm, globalObject, encodedOp1, encodedOp2, *arithProfile);
}

EncodedJSValue JIT_OPERATION operationValueSubOptimize(JSGlobalObject* globalObject, EncodedJSValue encodedOp1, EncodedJSValue encodedOp2, JITSubIC* subIC)
{
    VM& vm = globalObject->vm();
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);

    auto nonOptimizeVariant = operationValueSubNoOptimize;
    if (BinaryArithProfile* arithProfile = subIC->arithProfile())
        arithProfile->observeLHSAndRHS(JSValue::decode(encodedOp1), JSValue::decode(encodedOp2));
    subIC->generateOutOfLine(callFrame->codeBlock(), nonOptimizeVariant);

#if ENABLE(MATH_IC_STATS)
    callFrame->codeBlock()->dumpMathICStats();
#endif

    return unprofiledSub(globalObject, encodedOp1, encodedOp2);
}

EncodedJSValue JIT_OPERATION operationValueSubNoOptimize(JSGlobalObject* globalObject, EncodedJSValue encodedOp1, EncodedJSValue encodedOp2, JITSubIC*)
{
    VM& vm = globalObject->vm();
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);

    return unprofiledSub(globalObject, encodedOp1, encodedOp2);
}

EncodedJSValue JIT_OPERATION operationValueSubProfiledOptimize(JSGlobalObject* globalObject, EncodedJSValue encodedOp1, EncodedJSValue encodedOp2, JITSubIC* subIC)
{
    VM& vm = globalObject->vm();
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);

    BinaryArithProfile* arithProfile = subIC->arithProfile();
    ASSERT(arithProfile);
    arithProfile->observeLHSAndRHS(JSValue::decode(encodedOp1), JSValue::decode(encodedOp2));
    auto nonOptimizeVariant = operationValueSubProfiledNoOptimize;
    subIC->generateOutOfLine(callFrame->codeBlock(), nonOptimizeVariant);

#if ENABLE(MATH_IC_STATS)
    callFrame->codeBlock()->dumpMathICStats();
#endif

    return profiledSub(vm, globalObject, encodedOp1, encodedOp2, *arithProfile, false);
}

EncodedJSValue JIT_OPERATION operationValueSubProfiledNoOptimize(JSGlobalObject* globalObject, EncodedJSValue encodedOp1, EncodedJSValue encodedOp2, JITSubIC* subIC)
{
    VM& vm = globalObject->vm();
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);

    BinaryArithProfile* arithProfile = subIC->arithProfile();
    ASSERT(arithProfile);
    return profiledSub(vm, globalObject, encodedOp1, encodedOp2, *arithProfile);
}

void JIT_OPERATION operationProcessTypeProfilerLog(VM* vmPointer)
{
    VM& vm = *vmPointer;
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);
    vm.typeProfilerLog()->processLogEntries(vm, "Log Full, called from inside baseline JIT"_s);
}

void JIT_OPERATION operationProcessShadowChickenLog(VM* vmPointer)
{
    VM& vm = *vmPointer;
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);
    RELEASE_ASSERT(vm.shadowChicken());
    vm.shadowChicken()->update(vm, callFrame);
}

int32_t JIT_OPERATION operationCheckIfExceptionIsUncatchableAndNotifyProfiler(VM* vmPointer)
{
    VM& vm = *vmPointer;
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);
    auto scope = DECLARE_THROW_SCOPE(vm);
    RELEASE_ASSERT(!!scope.exception());

    if (isTerminatedExecutionException(vm, scope.exception())) {
        genericUnwind(vm, callFrame);
        return 1;
    }
    return 0;
}

} // extern "C"

} // namespace JSC

IGNORE_WARNINGS_END

#endif // ENABLE(JIT)
