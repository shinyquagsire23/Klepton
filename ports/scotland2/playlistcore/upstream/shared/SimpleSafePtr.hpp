#pragma once

#include "beatsaber-hook/shared/utils/il2cpp-functions.hpp"

#define NULL_CHECK(check, ret) CRASH_UNLESS(check); return ret;

template <class T>
struct SimpleSafePtr {
    SimpleSafePtr(){};

    SimpleSafePtr(T* instace) { internalWrapper = SimpleSafePtrWrapper::New(instace); }

    SimpleSafePtr(SimpleSafePtr const&) = delete;

    ~SimpleSafePtr() { il2cpp_functions::GC_free(internalWrapper); }

    inline SimpleSafePtr<T>& operator=(T* other) {
        if (!internalWrapper)
            internalWrapper = SimpleSafePtrWrapper::New(other);
        else
            internalWrapper->internalPointer = other;
        return *this;
    }

    T& operator*() { NULL_CHECK(internalWrapper, *internalWrapper->internalPointer); }

    T const& operator*() const { NULL_CHECK(internalWrapper, *internalWrapper->internalPointer); }

    T* const operator->() const { NULL_CHECK(internalWrapper, internalWrapper->internalPointer); }

    operator T* const() const { NULL_CHECK(internalWrapper, internalWrapper->internalPointer); }

    operator bool() const noexcept {
        if (!internalWrapper)
            return false;
        return (bool) internalWrapper->internalPointer;
    }

   private:
    struct SimpleSafePtrWrapper {
        static SimpleSafePtrWrapper* New(T* instance) {
            il2cpp_functions::Init();
            CRASH_UNLESS(il2cpp_functions::hasGCFuncs);
            auto wrapper = (SimpleSafePtrWrapper*) il2cpp_functions::GarbageCollector_AllocateFixed(sizeof(SimpleSafePtrWrapper), nullptr);
            CRASH_UNLESS(wrapper);
            wrapper->internalPointer = instance;
            return wrapper;
        }

        SimpleSafePtrWrapper() = delete;
        ~SimpleSafePtrWrapper() = delete;

        T* internalPointer;
    };
    SimpleSafePtrWrapper* internalWrapper;
};

#undef NULL_CHECK
