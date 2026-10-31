#pragma once

#include "custom-types/shared/macros.hpp"
#include "custom-types/shared/coroutine.hpp"
#include "UnityEngine/Coroutine.hpp"
#include "UnityEngine/MonoBehaviour.hpp"
#include "System/Collections/IEnumerator.hpp"

// equivalent to shared coroutine starter
DECLARE_CLASS_CODEGEN(BSML, SharedCoroutineStarter, UnityEngine::MonoBehaviour) {
        DECLARE_INSTANCE_METHOD(void, Awake);
    private:
        static SafePtrUnity<UnityEngine::MonoBehaviour> instance;
    public:
        static UnityEngine::MonoBehaviour* get_instance();

        static void StopCoroutine(UnityEngine::Coroutine* coroutine);
        static UnityEngine::Coroutine* StartCoroutine(::System::Collections::IEnumerator* enumerator);
        static UnityEngine::Coroutine* StartCoroutine(custom_types::Helpers::Coroutine coroutine);

};