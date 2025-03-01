/*
 * Copyright (C) 2016-2019 Apple Inc. All rights reserved.
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

#include "JSDestructibleObject.h"
#include "JSObject.h"
#include "WasmMemory.h"
#include <wtf/Ref.h>
#include <wtf/RefPtr.h>

namespace JSC {

class ArrayBuffer;
class JSArrayBuffer;

class JSWebAssemblyMemory final : public JSDestructibleObject {
public:
    typedef JSDestructibleObject Base;

    template<typename CellType, SubspaceAccess mode>
    static IsoSubspace* subspaceFor(VM& vm)
    {
        return vm.webAssemblyMemorySpace<mode>();
    }

    static JSWebAssemblyMemory* create(JSGlobalObject*, VM&, Structure*);
    static Structure* createStructure(VM&, JSGlobalObject*, JSValue);

    DECLARE_EXPORT_INFO;

    void adopt(Ref<Wasm::Memory>&&);
    Wasm::Memory& memory() { return m_memory.get(); }
    JSArrayBuffer* buffer(VM& vm, JSGlobalObject*);
    Wasm::PageCount grow(VM&, JSGlobalObject*, uint32_t delta);
    void growSuccessCallback(VM&, Wasm::PageCount oldPageCount, Wasm::PageCount newPageCount);

private:
    JSWebAssemblyMemory(VM&, Structure*);
    void finishCreation(VM&);
    static void destroy(JSCell*);
    static void visitChildren(JSCell*, SlotVisitor&);

    Ref<Wasm::Memory> m_memory;
    WriteBarrier<JSArrayBuffer> m_bufferWrapper;
    RefPtr<ArrayBuffer> m_buffer;
};

} // namespace JSC

#endif // ENABLE(WEBASSEMBLY)
