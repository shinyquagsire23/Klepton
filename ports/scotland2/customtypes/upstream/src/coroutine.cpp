#include "coroutine.hpp"
#include "logging.hpp"
#include "register.hpp"

namespace custom_types::Helpers {
DEFINE_TYPE(custom_types::Helpers, ResetableCoroutine);
DEFINE_TYPE(custom_types::Helpers, StandardCoroutine);

struct InternalHelper {
  static bool MoveNextHelper(Coroutine *&currentCoro, Wrapper &current) {
    if (!currentCoro) {
      // If we do not have a current coroutine, we exit immediately.
      return false;
    }
    // Each call to MoveNext performs the following:
    // If we have an object that is current, wait on it until complete,
    // fallthrough (this is performed by C#): If we have no object, advance our
    // own until we are complete, fallthough: If we are complete, return false

    // if (current) {
    //     // current will never be GC'd, as it is a field on a custom type.
    //     // current will also always be an enumeratorT. This will allow us to
    //     call RunMethod(current, "MoveNext") (possibly
    //     "System.Collections.IEnumerator.MoveNext") if
    //     (THROW_UNLESS(il2cpp_utils::RunMethod<bool>(current, "MoveNext"))) {
    //         // If we are waiting on our current object, wait until it is
    //         complete return true;
    //     }
    //     // Our current instance is complete, run ourselves now.
    //     current = nullptr;
    // }
    // reset value, we have nothing
    current = nullptr;
    currentCoro->m_coroutine.resume();
    if (currentCoro->m_coroutine.done()) {
      current.instance = nullptr;

      // this was originally here but it caused crashes in the tests and seems to be a memory leak anyways
      // currentCoro = nullptr;

      // TODO: See if there is ever a case where this caught exception would
      // result in the coroutine not properly being reset ex: current not being
      // set to nullptr, currentCoro not being set to nullptr
      if (currentCoro->m_coroutine.promise().failed()) {
          IL2CPP_CATCH_HANDLER(currentCoro->m_coroutine.promise().rethrow_if_exception();)
      }


      return false;
    }
    // Otherwise, get the next current value
    current = currentCoro->m_coroutine.promise().value();
    return true;
  }
};

void ResetableCoroutine::ctor(CoroFuncType *creator) {
  // Create copy
  coroCreator = *creator;
  currentCoro = new Coroutine(coroCreator());
  current = nullptr;
  // Valid is only set to false when this instance is destroyed to prevent
  // further calls.
  valid = true;
}

bool ResetableCoroutine::MoveNext() {
  if (!valid) {
    throw CoroutineDisposed();
  }
  return InternalHelper::MoveNextHelper(currentCoro, current);
}

Il2CppObject* ResetableCoroutine::get_Current() {
    // if (current.instance) {
    //     if (!current.instance->klass->initialized_and_no_error) {
    //         il2cpp_functions::Class_Init(current.instance->klass);
    //     }

    //     auto enumeratorTKlass = il2cpp_utils::GetClassFromName("System.Collections", "IEnumerator");

    //     custom_types::logger.debug("Unity asked for resettable value, we have {} {} {}", fmt::ptr(current.instance), il2cpp_utils::ClassStandardName(current.instance->klass, true),
    //                                   il2cpp_functions::class_is_assignable_from(enumeratorTKlass, current.instance->klass));
    // }
    return reinterpret_cast<Il2CppObject*>(current.instance);
}

void ResetableCoroutine::Reset() {
  if (!valid) {
    throw CoroutineDisposed();
  }
  // When we reset this coroutine, we need to destroy our current coroutine.
  // Then we need to get a new one
  // Then we need to set our current to nullptr
  if (currentCoro) {
    delete currentCoro;
  }
  currentCoro = new Coroutine(coroCreator());
  current = nullptr;
}

void ResetableCoroutine::Finalize() {
  // On destruction, we must destroy everything we have.
  valid = false;
  coroCreator.~CoroFuncType();
  delete currentCoro;
  // It does not matter what current is, as GC will no longer see this reference
  // and it will get cleaned up later.
}

void StandardCoroutine::ctor(Coroutine *coro) {
  current = nullptr;
  currentCoro = coro;
  valid = true;
}

bool StandardCoroutine::MoveNext() {
    if (!valid) {
      throw CoroutineDisposed();
  }
  return InternalHelper::MoveNextHelper(currentCoro, current);
}

Il2CppObject* StandardCoroutine::get_Current() {
    // if (current.instance) {
    //     auto enumeratorTKlass = il2cpp_utils::GetClassFromName("System.Collections", "IEnumerator");

    //     auto containsInterface = std::find(current.instance->klass->implementedInterfaces, current.instance->klass->implementedInterfaces + current.instance->klass->interfaces_count, enumeratorTKlass);

    //     custom_types::logger.debug("Unity asked for value, we have {} {} {}", fmt::ptr(current.instance), il2cpp_utils::ClassStandardName(current.instance->klass, true),
    //                                   il2cpp_functions::class_is_subclass_of(current.instance->klass, enumeratorTKlass, true));
    // }
    return reinterpret_cast<Il2CppObject*>(current.instance);
}

void StandardCoroutine::Reset() { throw CoroutineNotResettable(); }

void StandardCoroutine::Finalize() {
  // On destruction, we must destroy everything we have.
  valid = false;
  delete currentCoro;
  // It does not matter what current is, as GC will no longer see this reference
  // and it will get cleaned up later.
}

void CoroutineHelper::EnsureCoroutines() {
  static bool initialized = false;
  if (!initialized) {
    custom_types::logger.debug("Calling EnsureCoroutines... Attempting to "
                                  "initialize coro custom types");
    custom_types::Register::ExplicitRegister(
        {&__registration_instance_ResetableCoroutine,
         &__registration_instance_StandardCoroutine});
    initialized = true;
  }
}
} // namespace custom_types::Helpers
