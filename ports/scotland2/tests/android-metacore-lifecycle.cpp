// Called only after the original MetaCore late_load in the running Unity game.
#include "types.hpp"
#include "unity.hpp"
#include "UnityEngine/GameObject.hpp"
#include <cstdio>
static unsigned immediate_callbacks,deferred_callbacks;
extern "C" __attribute__((visibility("default")))
int sc2_test_metacore_late() {
    auto scheduler=UnityEngine::GameObject::Find("MetaCoreMainThread");
    if(!scheduler || !scheduler->GetComponent<MetaCore::MainThreadScheduler*>())return 1;
    auto immediate=UnityEngine::GameObject::New_ctor("Scotland2MetaCoreDestroyImmediateTest");
    auto expected_id=immediate->GetInstanceID();
    MetaCore::Engine::SetOnDestroy(immediate,[]{++immediate_callbacks;});
    UnityEngine::Object::DestroyImmediate(immediate);
    std::fprintf(stderr,"[scotland2] METACORE_DESTROY_IMMEDIATE_RESULT instanceId=%d callbacks=%u\n",expected_id,immediate_callbacks);
    if(immediate_callbacks!=1)return 2;
    std::fprintf(stderr,"[scotland2] METACORE_DESTROY_IMMEDIATE_OK original SetOnDestroy callback fired once through the measured native Unity deletion entry\n");
    auto deferred=UnityEngine::GameObject::New_ctor("Scotland2MetaCoreDestroyDeferredTest");
    MetaCore::Engine::SetOnDestroy(deferred,[]{++deferred_callbacks;});
    UnityEngine::Object::Destroy(deferred);
    MetaCore::MainThreadScheduler::Schedule([] {
        std::fprintf(stderr,"[scotland2] METACORE_UPDATE_OK original Unity component Update dispatched a scheduled callback\n");
    });
    MetaCore::MainThreadScheduler::Schedule([]{return deferred_callbacks!=0;},[] {
        std::fprintf(stderr,"[scotland2] METACORE_DESTROY_DEFERRED_%s original delayed Unity destruction callback count=%u\n",deferred_callbacks==1?"OK":"FAILED",deferred_callbacks);
    });
    return 0;
}
