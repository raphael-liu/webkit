/*
 * Copyright (C) 2019 Apple Inc. All rights reserved.
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

#pragma once

#if ENABLE(WEBASSEMBLY)

#include "WasmModuleInformation.h"
#include "WasmPlan.h"
#include "WasmStreamingParser.h"
#include <wtf/Function.h>
#include <wtf/SharedTask.h>
#include <wtf/ThreadSafeRefCounted.h>
#include <wtf/Vector.h>

namespace JSC {

class CallLinkInfo;

namespace Wasm {

class EntryPlan : public Plan, public StreamingParserClient {
public:
    using Base = Plan;
    enum AsyncWork : uint8_t { FullCompile, Validation };

    // Note: CompletionTask should not hold a reference to the Plan otherwise there will be a reference cycle.
    EntryPlan(Context*, Ref<ModuleInformation>, AsyncWork, CompletionTask&&, CreateEmbedderWrapper&&, ThrowWasmException);
    JS_EXPORT_PRIVATE EntryPlan(Context*, Vector<uint8_t>&&, AsyncWork, CompletionTask&&, CreateEmbedderWrapper&&, ThrowWasmException);
    EntryPlan(Context*, Ref<ModuleInformation>, CompletionTask&&);
    EntryPlan(Context*, AsyncWork, CompletionTask&&);

    virtual ~EntryPlan() = default;

    using CalleeInitializer = Function<void(uint32_t, RefPtr<Callee>&&, Ref<Callee>&&)>;
    virtual void initializeCallees(const CalleeInitializer&) = 0;

    bool parseAndValidateModule()
    {
        return parseAndValidateModule(m_source.data(), m_source.size());
    }
    bool parseAndValidateModule(const uint8_t*, size_t);

    void compileFunctions(CompilationEffort);

    Vector<Export>& exports() const
    {
        RELEASE_ASSERT(!failed() && !hasWork());
        return m_moduleInformation->exports;
    }

    size_t internalFunctionCount() const
    {
        RELEASE_ASSERT(!failed() && !hasWork());
        return m_moduleInformation->internalFunctionCount();
    }

    Ref<ModuleInformation>&& takeModuleInformation()
    {
        RELEASE_ASSERT(!failed() && !hasWork());
        return WTFMove(m_moduleInformation);
    }

    Vector<MacroAssemblerCodeRef<WasmEntryPtrTag>>&& takeWasmToWasmExitStubs()
    {
        RELEASE_ASSERT(!failed() && !hasWork());
        return WTFMove(m_wasmToWasmExitStubs);
    }

    Vector<Vector<UnlinkedWasmToWasmCall>> takeWasmToWasmCallsites()
    {
        RELEASE_ASSERT(!failed() && !hasWork());
        return WTFMove(m_unlinkedWasmToWasmCalls);
    }

    enum class State : uint8_t {
        Initial,
        Validated,
        Prepared,
        Compiled,
        Completed // We should only move to Completed if we are holding the lock.
    };

    bool hasWork() const override
    {
        if (m_asyncWork == AsyncWork::Validation)
            return m_state < State::Validated;
        return m_state < State::Compiled;
    }

    void work(CompilationEffort) override;
    bool hasBeenPrepared() const { return m_state >= State::Prepared; }
    bool multiThreaded() const override { return hasBeenPrepared(); }

    bool didReceiveFunctionData(unsigned, const FunctionData&) override;

private:
    class ThreadCountHolder;
    friend class ThreadCountHolder;

    void prepare();
    bool isComplete() const override { return m_state == State::Completed; }
    void complete(const AbstractLocker&) override;

    const char* stateString(State);

protected:
    // For some reason friendship doesn't extend to parent classes...
    using Base::m_lock;

    virtual bool prepareImpl() = 0;
    virtual void compileFunction(uint32_t functionIndex) = 0;
    virtual void didCompleteCompilation(const AbstractLocker&) = 0;

    void moveToState(State);

    template<typename T>
    bool tryReserveCapacity(Vector<T>& vector, size_t size, const char* what)
    {
        if (UNLIKELY(!vector.tryReserveCapacity(size))) {
            fail(holdLock(m_lock), WTF::makeString("Failed allocating enough space for ", size, what));
            return false;
        }
        return true;
    }

    Vector<uint8_t> m_source;
    Vector<MacroAssemblerCodeRef<WasmEntryPtrTag>> m_wasmToWasmExitStubs;
    HashSet<uint32_t, typename DefaultHash<uint32_t>::Hash, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_exportedFunctionIndices;

    Vector<Vector<UnlinkedWasmToWasmCall>> m_unlinkedWasmToWasmCalls;
    StreamingParser m_streamingParser;
    State m_state;

    const AsyncWork m_asyncWork;
    uint8_t m_numberOfActiveThreads { 0 };
    uint32_t m_currentIndex { 0 };
};


} } // namespace JSC::Wasm

#endif // ENABLE(WEBASSEMBLY)
