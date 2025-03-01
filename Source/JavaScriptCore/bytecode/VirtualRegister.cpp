/*
 * Copyright (C) 2011-2019 Apple Inc. All rights reserved.
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
#include "VirtualRegister.h"

#include "RegisterID.h"

namespace JSC {

void VirtualRegister::dump(PrintStream& out) const
{
    if (!isValid()) {
        out.print("<invalid>");
        return;
    }
    
    if (isHeader()) {
        if (m_virtualRegister == CallFrameSlot::codeBlock)
            out.print("codeBlock");
        else if (m_virtualRegister == CallFrameSlot::callee)
            out.print("callee");
        else if (m_virtualRegister == CallFrameSlot::argumentCount)
            out.print("argumentCount");
#if CPU(ADDRESS64)
        else if (!m_virtualRegister)
            out.print("callerFrame");
        else if (m_virtualRegister == 1)
            out.print("returnPC");
#else
        else if (!m_virtualRegister)
            out.print("callerFrameAndReturnPC");
#endif
        return;
    }
    
    if (isConstant()) {
        out.print("const", toConstantIndex());
        return;
    }
    
    if (isArgument()) {
        if (!toArgument())
            out.print("this");
        else
            out.print("arg", toArgument());
        return;
    }
    
    if (isLocal()) {
        out.print("loc", toLocal());
        return;
    }
    
    RELEASE_ASSERT_NOT_REACHED();
}


VirtualRegister::VirtualRegister(RegisterID* reg)
    : VirtualRegister(reg->m_virtualRegister.m_virtualRegister)
{
}

VirtualRegister::VirtualRegister(RefPtr<RegisterID> reg)
    : VirtualRegister(reg.get())
{
}

} // namespace JSC
