/*
 * Copyright (C) 2016 Canon, Inc. All rights reserved.
 * Copyright (C) 2016-2017 Apple Inc. All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY CANON INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL CANON INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#include "JSDOMConvert.h"
#include <JavaScriptCore/IteratorPrototype.h>
#include <type_traits>

namespace WebCore {

void addValueIterableMethods(JSC::JSGlobalObject&, JSC::JSObject&);

enum class JSDOMIteratorType { Set, Map };

// struct IteratorTraits {
//     static constexpr JSDOMIteratorType type = [Map|Set];
//     using KeyType = [IDLType|void];
//     using ValueType = [IDLType];
// };

template<typename T, typename U = void> using EnableIfMap = typename std::enable_if<T::type == JSDOMIteratorType::Map, U>::type;
template<typename T, typename U = void> using EnableIfSet = typename std::enable_if<T::type == JSDOMIteratorType::Set, U>::type;

template<typename JSWrapper, typename IteratorTraits> class JSDOMIteratorPrototype : public JSC::JSNonFinalObject {
public:
    using Base = JSC::JSNonFinalObject;
    using DOMWrapped = typename JSWrapper::DOMWrapped;

    static JSDOMIteratorPrototype* create(JSC::VM& vm, JSC::JSGlobalObject* globalObject, JSC::Structure* structure)
    {
        JSDOMIteratorPrototype* prototype = new (NotNull, JSC::allocateCell<JSDOMIteratorPrototype>(vm.heap)) JSDOMIteratorPrototype(vm, structure);
        prototype->finishCreation(vm, globalObject);
        return prototype;
    }

    DECLARE_INFO;

    static JSC::Structure* createStructure(JSC::VM& vm, JSC::JSGlobalObject* globalObject, JSC::JSValue prototype)
    {
        return JSC::Structure::create(vm, globalObject, prototype, JSC::TypeInfo(JSC::ObjectType, StructureFlags), info());
    }

    static JSC::EncodedJSValue JSC_HOST_CALL next(JSC::JSGlobalObject*, JSC::CallFrame*);

private:
    JSDOMIteratorPrototype(JSC::VM& vm, JSC::Structure* structure) : Base(vm, structure) { }

    void finishCreation(JSC::VM&, JSC::JSGlobalObject*);
};

enum class IterationKind { Key, Value, KeyValue };

template<typename JSWrapper, typename IteratorTraits> class JSDOMIterator : public JSDOMObject {
public:
    using Base = JSDOMObject;

    using Wrapper = JSWrapper;
    using Traits = IteratorTraits;

    using DOMWrapped = typename Wrapper::DOMWrapped;
    using Prototype = JSDOMIteratorPrototype<Wrapper, Traits>;

    DECLARE_INFO;

    static JSC::Structure* createStructure(JSC::VM& vm, JSC::JSGlobalObject* globalObject, JSC::JSValue prototype)
    {
        return JSC::Structure::create(vm, globalObject, prototype, JSC::TypeInfo(JSC::ObjectType, StructureFlags), info());
    }

    static JSDOMIterator* create(JSC::VM& vm, JSC::Structure* structure, JSWrapper& iteratedObject, IterationKind kind)
    {
        JSDOMIterator* instance = new (NotNull, JSC::allocateCell<JSDOMIterator>(vm.heap)) JSDOMIterator(structure, iteratedObject, kind);
        instance->finishCreation(vm);
        return instance;
    }

    static Prototype* createPrototype(JSC::VM& vm, JSC::JSGlobalObject& globalObject)
    {
        return Prototype::create(vm, &globalObject, Prototype::createStructure(vm, &globalObject, globalObject.iteratorPrototype()));
    }

    JSC::JSValue next(JSC::JSGlobalObject&);

private:
    JSDOMIterator(JSC::Structure* structure, JSWrapper& iteratedObject, IterationKind kind)
        : Base(structure, *iteratedObject.globalObject())
        , m_iterator(iteratedObject.wrapped().createIterator())
        , m_kind(kind)
    {
    }

    template<typename IteratorValue, typename T = Traits> EnableIfMap<T, JSC::JSValue> asJS(JSC::JSGlobalObject&, IteratorValue&);
    template<typename IteratorValue, typename T = Traits> EnableIfSet<T, JSC::JSValue> asJS(JSC::JSGlobalObject&, IteratorValue&);

    static void destroy(JSC::JSCell*);

    Optional<typename DOMWrapped::Iterator> m_iterator;
    IterationKind m_kind;
};

inline JSC::JSValue jsPair(JSC::JSGlobalObject&, JSDOMGlobalObject& globalObject, JSC::JSValue value1, JSC::JSValue value2)
{
    JSC::MarkedArgumentBuffer arguments;
    arguments.append(value1);
    arguments.append(value2);
    ASSERT(!arguments.hasOverflowed());
    return constructArray(&globalObject, static_cast<JSC::ArrayAllocationProfile*>(nullptr), arguments);
}

template<typename FirstType, typename SecondType, typename T, typename U> 
inline JSC::JSValue jsPair(JSC::JSGlobalObject& lexicalGlobalObject, JSDOMGlobalObject& globalObject, const T& value1, const U& value2)
{
    return jsPair(lexicalGlobalObject, globalObject, toJS<FirstType>(lexicalGlobalObject, globalObject, value1), toJS<SecondType>(lexicalGlobalObject, globalObject, value2));
}

template<typename JSIterator> JSC::JSValue iteratorCreate(typename JSIterator::Wrapper&, IterationKind);
template<typename JSIterator> JSC::JSValue iteratorForEach(JSC::JSGlobalObject&, JSC::CallFrame&, typename JSIterator::Wrapper&, JSC::ThrowScope&);

template<typename JSIterator> JSC::JSValue iteratorCreate(typename JSIterator::Wrapper& thisObject, IterationKind kind)
{
    ASSERT(thisObject.globalObject());
    JSDOMGlobalObject& globalObject = *thisObject.globalObject();
    return JSIterator::create(globalObject.vm(), getDOMStructure<JSIterator>(globalObject.vm(), globalObject), thisObject, kind);
}

template<typename JSWrapper, typename IteratorTraits>
template<typename IteratorValue, typename T> inline EnableIfMap<T, JSC::JSValue> JSDOMIterator<JSWrapper, IteratorTraits>::asJS(JSC::JSGlobalObject& lexicalGlobalObject, IteratorValue& value)
{
    ASSERT(value);
    
    switch (m_kind) {
    case IterationKind::Key:
        return toJS<typename Traits::KeyType>(lexicalGlobalObject, *globalObject(), value->key);
    case IterationKind::Value:
        return toJS<typename Traits::ValueType>(lexicalGlobalObject, *globalObject(), value->value);
    case IterationKind::KeyValue:
        return jsPair<typename Traits::KeyType, typename Traits::ValueType>(lexicalGlobalObject, *globalObject(), value->key, value->value);
    };
    
    ASSERT_NOT_REACHED();
    return { };
}

template<typename JSWrapper, typename IteratorTraits>
template<typename IteratorValue, typename T> inline EnableIfSet<T, JSC::JSValue> JSDOMIterator<JSWrapper, IteratorTraits>::asJS(JSC::JSGlobalObject& lexicalGlobalObject, IteratorValue& value)
{
    ASSERT(value);

    auto globalObject = this->globalObject();
    auto result = toJS<typename Traits::ValueType>(lexicalGlobalObject, *globalObject, value);

    switch (m_kind) {
    case IterationKind::Key:
    case IterationKind::Value:
        return result;
    case IterationKind::KeyValue:
        return jsPair(lexicalGlobalObject, *globalObject, result, result);
    };

    ASSERT_NOT_REACHED();
    return { };
}

template<typename JSIterator, typename IteratorValue> EnableIfMap<typename JSIterator::Traits> appendForEachArguments(JSC::JSGlobalObject& lexicalGlobalObject, JSDOMGlobalObject& globalObject, JSC::MarkedArgumentBuffer& arguments, IteratorValue& value)
{
    ASSERT(value);
    arguments.append(toJS<typename JSIterator::Traits::ValueType>(lexicalGlobalObject, globalObject, value->value));
    arguments.append(toJS<typename JSIterator::Traits::KeyType>(lexicalGlobalObject, globalObject, value->key));
}

template<typename JSIterator, typename IteratorValue> EnableIfSet<typename JSIterator::Traits> appendForEachArguments(JSC::JSGlobalObject& lexicalGlobalObject, JSDOMGlobalObject& globalObject, JSC::MarkedArgumentBuffer& arguments, IteratorValue& value)
{
    ASSERT(value);
    auto argument = toJS<typename JSIterator::Traits::ValueType>(lexicalGlobalObject, globalObject, value);
    arguments.append(argument);
    arguments.append(argument);
}

template<typename JSIterator> JSC::JSValue iteratorForEach(JSC::JSGlobalObject& lexicalGlobalObject, JSC::CallFrame& callFrame, typename JSIterator::Wrapper& thisObject, JSC::ThrowScope& scope)
{
    JSC::JSValue callback = callFrame.argument(0);
    JSC::JSValue thisValue = callFrame.argument(1);

    JSC::CallData callData;
    JSC::CallType callType = JSC::getCallData(JSC::getVM(&lexicalGlobalObject), callback, callData);
    if (callType == JSC::CallType::None)
        return throwTypeError(&lexicalGlobalObject, scope, "Cannot call callback"_s);

    auto iterator = thisObject.wrapped().createIterator();
    while (auto value = iterator.next()) {
        JSC::MarkedArgumentBuffer arguments;
        appendForEachArguments<JSIterator>(lexicalGlobalObject, *thisObject.globalObject(), arguments, value);
        arguments.append(&thisObject);
        if (UNLIKELY(arguments.hasOverflowed())) {
            throwOutOfMemoryError(&lexicalGlobalObject, scope);
            return { };
        }
        JSC::call(&lexicalGlobalObject, callback, callType, callData, thisValue, arguments);
        if (UNLIKELY(scope.exception()))
            break;
    }
    return JSC::jsUndefined();
}

template<typename JSWrapper, typename IteratorTraits>
void JSDOMIterator<JSWrapper, IteratorTraits>::destroy(JSCell* cell)
{
    JSDOMIterator<JSWrapper, IteratorTraits>* thisObject = static_cast<JSDOMIterator<JSWrapper, IteratorTraits>*>(cell);
    thisObject->JSDOMIterator<JSWrapper, IteratorTraits>::~JSDOMIterator();
}

template<typename JSWrapper, typename IteratorTraits>
JSC::JSValue JSDOMIterator<JSWrapper, IteratorTraits>::next(JSC::JSGlobalObject& lexicalGlobalObject)
{
    if (m_iterator) {
        auto iteratorValue = m_iterator->next();
        if (iteratorValue)
            return createIteratorResultObject(&lexicalGlobalObject, asJS(lexicalGlobalObject, iteratorValue), false);
        m_iterator = WTF::nullopt;
    }
    return createIteratorResultObject(&lexicalGlobalObject, JSC::jsUndefined(), true);
}

template<typename JSWrapper, typename IteratorTraits>
JSC::EncodedJSValue JSC_HOST_CALL JSDOMIteratorPrototype<JSWrapper, IteratorTraits>::next(JSC::JSGlobalObject* globalObject, JSC::CallFrame* callFrame)
{
    JSC::VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    auto iterator = JSC::jsDynamicCast<JSDOMIterator<JSWrapper, IteratorTraits>*>(vm, callFrame->thisValue());
    if (!iterator)
        return JSC::JSValue::encode(throwTypeError(globalObject, scope, "Cannot call next() on a non-Iterator object"_s));

    return JSC::JSValue::encode(iterator->next(*globalObject));
}

template<typename JSWrapper, typename IteratorTraits>
void JSDOMIteratorPrototype<JSWrapper, IteratorTraits>::finishCreation(JSC::VM& vm, JSC::JSGlobalObject* globalObject)
{
    Base::finishCreation(vm);
    ASSERT(inherits(vm, info()));

    JSC_NATIVE_INTRINSIC_FUNCTION_WITHOUT_TRANSITION(vm.propertyNames->next, next, 0, 0, JSC::NoIntrinsic);
}

}
