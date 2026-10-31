#include "BSML/SharedCoroutineStarter.hpp"

#include "UnityEngine/GameObject.hpp"

DEFINE_TYPE(BSML, SharedCoroutineStarter);

namespace BSML {
    SafePtrUnity<UnityEngine::MonoBehaviour> SharedCoroutineStarter::instance;

    UnityEngine::MonoBehaviour* SharedCoroutineStarter::get_instance() {
        return instance.ptr();
    }

    void SharedCoroutineStarter::Awake() {
        instance = this;
    }

    void SharedCoroutineStarter::StopCoroutine(UnityEngine::Coroutine* coroutine) {
        instance->StopCoroutine(coroutine);
    }

    UnityEngine::Coroutine* SharedCoroutineStarter::StartCoroutine(::System::Collections::IEnumerator* enumerator) {
        return instance->StartCoroutine(enumerator);
    }

    UnityEngine::Coroutine* SharedCoroutineStarter::StartCoroutine(custom_types::Helpers::Coroutine coroutine) {
        return StartCoroutine(custom_types::Helpers::CoroutineHelper::New(std::move(coroutine)));
    }
}
