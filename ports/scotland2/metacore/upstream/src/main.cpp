#include "main.hpp"

#include "UnityEngine/GameObject.hpp"
#include "events.hpp"
#include "hooks.hpp"
#include "input.hpp"
#include "scotland2/shared/modloader.h"
#include "types.hpp"

static modloader::ModInfo modInfo = {MOD_ID, VERSION, 0};

static void RegisterButtonEvents() {
    for (int i = 0; i <= MetaCore::Input::ButtonsMax; i++) {
        MetaCore::Events::RegisterEvent(MetaCore::Input::PressEvents, i);
        MetaCore::Events::RegisterEvent(MetaCore::Input::ReleaseEvents, i);
        MetaCore::Events::RegisterEvent(MetaCore::Input::HoldEvents, i);
    }
}

extern "C" METACORE_EXPORT void setup(CModInfo* info) {
    *info = modInfo.to_c();
    Paper::Logger::RegisterFileContextId(MOD_ID);
    RegisterButtonEvents();
    logger.info("Completed setup!");
}

extern "C" METACORE_EXPORT void late_load() {
    il2cpp_functions::Init();
    custom_types::Register::AutoRegister();
    Hooks::Install();

    auto mainThread = UnityEngine::GameObject::New_ctor("MetaCoreMainThread");
    UnityEngine::Object::DontDestroyOnLoad(mainThread);
    mainThread->AddComponent<MetaCore::MainThreadScheduler*>();
}
