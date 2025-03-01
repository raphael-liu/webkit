/*
 * Copyright (C) 2008-2018 Apple Inc. All rights reserved.
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

#include "CodeBlock.h"
#include "CodeOrigin.h"
#include "Instruction.h"
#include "JITStubRoutine.h"
#include "MacroAssembler.h"
#include "Options.h"
#include "RegisterSet.h"
#include "Structure.h"
#include "StructureSet.h"
#include "StructureStubClearingWatchpoint.h"
#include "StubInfoSummary.h"
#include <wtf/Box.h>

namespace JSC {

#if ENABLE(JIT)

class AccessCase;
class AccessGenerationResult;
class PolymorphicAccess;

enum class AccessType : int8_t {
    GetById,
    GetByIdWithThis,
    GetByIdDirect,
    TryGetById,
    GetByVal,
    Put,
    In,
    InstanceOf
};

enum class CacheType : int8_t {
    Unset,
    GetByIdSelf,
    PutByIdReplace,
    InByIdSelf,
    Stub,
    ArrayLength,
    StringLength
};

class StructureStubInfo {
    WTF_MAKE_NONCOPYABLE(StructureStubInfo);
    WTF_MAKE_FAST_ALLOCATED;
public:
    StructureStubInfo(AccessType);
    ~StructureStubInfo();

    void initGetByIdSelf(CodeBlock*, Structure* baseObjectStructure, PropertyOffset, const Identifier&);
    void initArrayLength();
    void initStringLength();
    void initPutByIdReplace(CodeBlock*, Structure* baseObjectStructure, PropertyOffset);
    void initInByIdSelf(CodeBlock*, Structure* baseObjectStructure, PropertyOffset);

    AccessGenerationResult addAccessCase(const GCSafeConcurrentJSLocker&, CodeBlock*, const Identifier&, std::unique_ptr<AccessCase>);

    void reset(CodeBlock*);

    void deref();
    void aboutToDie();

    // Check if the stub has weak references that are dead. If it does, then it resets itself,
    // either entirely or just enough to ensure that those dead pointers don't get used anymore.
    void visitWeakReferences(CodeBlock*);
    
    // This returns true if it has marked everything that it will ever mark.
    bool propagateTransitions(SlotVisitor&);
        
    ALWAYS_INLINE bool considerCaching(VM& vm, CodeBlock* codeBlock, Structure* structure, UniquedStringImpl* impl = nullptr)
    {
        DisallowGC disallowGC;

        // We never cache non-cells.
        if (!structure) {
            sawNonCell = true;
            return false;
        }
        
        // This method is called from the Optimize variants of IC slow paths. The first part of this
        // method tries to determine if the Optimize variant should really behave like the
        // non-Optimize variant and leave the IC untouched.
        //
        // If we determine that we should do something to the IC then the next order of business is
        // to determine if this Structure would impact the IC at all. We know that it won't, if we
        // have already buffered something on its behalf. That's what the bufferedStructures set is
        // for.
        
        everConsidered = true;
        if (!countdown) {
            // Check if we have been doing repatching too frequently. If so, then we should cool off
            // for a while.
            WTF::incrementWithSaturation(repatchCount);
            if (repatchCount > Options::repatchCountForCoolDown()) {
                // We've been repatching too much, so don't do it now.
                repatchCount = 0;
                // The amount of time we require for cool-down depends on the number of times we've
                // had to cool down in the past. The relationship is exponential. The max value we
                // allow here is 2^256 - 2, since the slow paths may increment the count to indicate
                // that they'd like to temporarily skip patching just this once.
                countdown = WTF::leftShiftWithSaturation(
                    static_cast<uint8_t>(Options::initialCoolDownCount()),
                    numberOfCoolDowns,
                    static_cast<uint8_t>(std::numeric_limits<uint8_t>::max() - 1));
                WTF::incrementWithSaturation(numberOfCoolDowns);
                
                // We may still have had something buffered. Trigger generation now.
                bufferingCountdown = 0;
                return true;
            }
            
            // We don't want to return false due to buffering indefinitely.
            if (!bufferingCountdown) {
                // Note that when this returns true, it's possible that we will not even get an
                // AccessCase because this may cause Repatch.cpp to simply do an in-place
                // repatching.
                return true;
            }
            
            bufferingCountdown--;
            
            // Now protect the IC buffering. We want to proceed only if this is a structure that
            // we don't already have a case buffered for. Note that if this returns true but the
            // bufferingCountdown is not zero then we will buffer the access case for later without
            // immediately generating code for it.
            //
            // NOTE: This will behave oddly for InstanceOf if the user varies the prototype but not
            // the base's structure. That seems unlikely for the canonical use of instanceof, where
            // the prototype is fixed.
            bool isNewlyAdded = bufferedStructures.add({ structure, impl }).isNewEntry;
            if (isNewlyAdded)
                vm.heap.writeBarrier(codeBlock);
            return isNewlyAdded;
        }
        countdown--;
        return false;
    }

    StubInfoSummary summary() const;
    
    static StubInfoSummary summary(const StructureStubInfo*);

    bool containsPC(void* pc) const;

    CodeOrigin codeOrigin;
private:
    Box<Identifier> m_getByIdSelfIdentifier;
public:

    union {
        struct {
            WriteBarrierBase<Structure> baseObjectStructure;
            PropertyOffset offset;
        } byIdSelf;
        PolymorphicAccess* stub;
    } u;

    Box<Identifier> getByIdSelfIdentifier()
    {
        RELEASE_ASSERT(m_cacheType == CacheType::GetByIdSelf);
        return m_getByIdSelfIdentifier;
    }
    
private:
    // Represents those structures that already have buffered AccessCases in the PolymorphicAccess.
    // Note that it's always safe to clear this. If we clear it prematurely, then if we see the same
    // structure again during this buffering countdown, we will create an AccessCase object for it.
    // That's not so bad - we'll get rid of the redundant ones once we regenerate.
    HashSet<std::pair<Structure*, RefPtr<UniquedStringImpl>>> bufferedStructures;
public:
    
    struct {
        CodeLocationLabel<JITStubRoutinePtrTag> start; // This is either the start of the inline IC for *byId caches. or the location of patchable jump for 'instanceof' caches.
        CodeLocationLabel<JSInternalPtrTag> doneLocation;
        CodeLocationCall<JSInternalPtrTag> slowPathCallLocation;
        CodeLocationLabel<JITStubRoutinePtrTag> slowPathStartLocation;

        RegisterSet usedRegisters;

        uint32_t inlineSize() const
        {
            int32_t inlineSize = MacroAssembler::differenceBetweenCodePtr(start, doneLocation);
            ASSERT(inlineSize >= 0);
            return inlineSize;
        }

        GPRReg baseGPR;
        GPRReg valueGPR;
        union {
            GPRReg thisGPR;
            GPRReg prototypeGPR;
            GPRReg propertyGPR;
        } u;
#if USE(JSVALUE32_64)
        GPRReg valueTagGPR;
        GPRReg baseTagGPR;
        GPRReg thisTagGPR;
#endif
    } patch;

    GPRReg baseGPR() const
    {
        return patch.baseGPR;
    }

    CodeLocationCall<JSInternalPtrTag> slowPathCallLocation() { return patch.slowPathCallLocation; }
    CodeLocationLabel<JSInternalPtrTag> doneLocation() { return patch.doneLocation; }
    CodeLocationLabel<JITStubRoutinePtrTag> slowPathStartLocation() { return patch.slowPathStartLocation; }

    CodeLocationJump<JSInternalPtrTag> patchableJump()
    { 
        ASSERT(accessType == AccessType::InstanceOf);
        return patch.start.jumpAtOffset<JSInternalPtrTag>(0);
    }

    JSValueRegs valueRegs() const
    {
        return JSValueRegs(
#if USE(JSVALUE32_64)
            patch.valueTagGPR,
#endif
            patch.valueGPR);
    }

    bool thisValueIsInThisGPR() const { return accessType == AccessType::GetByIdWithThis; }

#if !ASSERT_DISABLED
    void checkConsistency();
#else
    ALWAYS_INLINE void checkConsistency() { }
#endif

    AccessType accessType;
private:
    CacheType m_cacheType;
    void setCacheType(CacheType);
public:
    CacheType cacheType() const { return m_cacheType; }
    uint8_t countdown; // We repatch only when this is zero. If not zero, we decrement.
    uint8_t repatchCount;
    uint8_t numberOfCoolDowns;

    CallSiteIndex callSiteIndex;

    uint8_t bufferingCountdown;
    bool resetByGC : 1;
    bool tookSlowPath : 1;
    bool everConsidered : 1;
    bool prototypeIsKnownObject : 1; // Only relevant for InstanceOf.
    bool sawNonCell : 1;
    bool hasConstantIdentifier : 1;
    bool propertyIsString : 1;
    bool propertyIsInt32 : 1;
    bool propertyIsSymbol : 1;
};

inline CodeOrigin getStructureStubInfoCodeOrigin(StructureStubInfo& structureStubInfo)
{
    return structureStubInfo.codeOrigin;
}

inline auto appropriateOptimizingGetByIdFunction(AccessType type) -> decltype(&operationGetByIdOptimize)
{
    switch (type) {
    case AccessType::GetById:
        return operationGetByIdOptimize;
    case AccessType::TryGetById:
        return operationTryGetByIdOptimize;
    case AccessType::GetByIdDirect:
        return operationGetByIdDirectOptimize;
    case AccessType::GetByIdWithThis:
    default:
        ASSERT_NOT_REACHED();
        return nullptr;
    }
}

inline auto appropriateGenericGetByIdFunction(AccessType type) -> decltype(&operationGetByIdGeneric)
{
    switch (type) {
    case AccessType::GetById:
        return operationGetByIdGeneric;
    case AccessType::TryGetById:
        return operationTryGetByIdGeneric;
    case AccessType::GetByIdDirect:
        return operationGetByIdDirectGeneric;
    case AccessType::GetByIdWithThis:
    default:
        ASSERT_NOT_REACHED();
        return nullptr;
    }
}

#else

class StructureStubInfo;

#endif // ENABLE(JIT)

typedef HashMap<CodeOrigin, StructureStubInfo*, CodeOriginApproximateHash> StubInfoMap;

} // namespace JSC
