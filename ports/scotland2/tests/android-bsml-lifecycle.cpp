// Independent consumer: observe original BSML late-created objects and Update.
#include "BSML/MainThreadScheduler.hpp"
#include "BSML/SharedCoroutineStarter.hpp"
#include "UnityEngine/GameObject.hpp"
#include <cstdio>
extern "C" __attribute__((visibility("default")))
int sc2_test_bsml_late() {
    if (!BSML::MainThreadScheduler::CurrentThreadIsMainThread()) return 1;
    auto scheduler=UnityEngine::GameObject::Find("BSMLMainThreadScheduler");
    auto starter=UnityEngine::GameObject::Find("BSMLSharedCoroutineStarter");
    if (!scheduler || !starter) return 2;
    auto component=scheduler->GetComponent<BSML::MainThreadScheduler*>();
    auto coroutine=starter->GetComponent<BSML::SharedCoroutineStarter*>();
    if (!component || !coroutine || BSML::SharedCoroutineStarter::get_instance()!=coroutine) return 3;
    BSML::MainThreadScheduler::ScheduleNextFrame([] {
        std::fprintf(stderr,"[scotland2] BSML_UPDATE_OK original Unity component Update dispatched a scheduled callback\n");
    });
    return 0;
}
