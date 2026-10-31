#include "BSML/MainThreadScheduler.hpp"
#include "scotland2/shared/loader.hpp"
#include "beatsaber-hook/shared/utils/typedefs.h"
#include "_config.h"
#include "hooking.hpp"
#include "logging.hpp"
#include "git_info.h"

#include "BSMLDataCache.hpp"
#include "assets.hpp"
#include "config.hpp"
#include "custom-types/shared/register.hpp"

#include "BSML/MainThreadScheduler.hpp"
#include "BSML/SharedCoroutineStarter.hpp"
#include "UnityEngine/GameObject.hpp"
#include "UnityEngine/Object.hpp"
#include "UnityEngine/HideFlags.hpp"

namespace BSML {}
using namespace BSML;

modloader::ModInfo modInfo{MOD_ID, VERSION, GIT_COMMIT};

BSML_EXPORT_FUNC void setup(CModInfo* info) {
    info->version = VERSION;
    info->id = MOD_ID;
    info->version_long = GIT_COMMIT;

    modInfo.assign(*info);
}

static bool isLoaded = false;
static bool isLateLoaded = false;

BSML_EXPORT_FUNC void load() {
    if (isLoaded) return;
    isLoaded = true;

    INFO("Loading BSML built from branch '" GIT_BRANCH "' and commit {}", GIT_COMMIT);

    if (!LoadConfig())
        SaveConfig();
    custom_types::Register::AutoRegister();
    Hooks::InstallHooks();
}

static constexpr inline UnityEngine::HideFlags operator |(UnityEngine::HideFlags a, UnityEngine::HideFlags b) {
    return UnityEngine::HideFlags(a.value__ | b.value__);
}

BSML_EXPORT_FUNC void late_load() {
    if (isLateLoaded) return;
    isLateLoaded = true;
    // late load is on main thread and really early, great time to setup these singletons
    auto mts = UnityEngine::GameObject::New_ctor("BSMLMainThreadScheduler");
    UnityEngine::Object::DontDestroyOnLoad(mts);
    mts->hideFlags = UnityEngine::HideFlags::DontUnloadUnusedAsset | UnityEngine::HideFlags::HideAndDontSave;
    mts->AddComponent<BSML::MainThreadScheduler*>();

    auto scs = UnityEngine::GameObject::New_ctor("BSMLSharedCoroutineStarter");
    UnityEngine::Object::DontDestroyOnLoad(scs);
    scs->hideFlags = UnityEngine::HideFlags::DontUnloadUnusedAsset | UnityEngine::HideFlags::HideAndDontSave;
    scs->AddComponent<BSML::SharedCoroutineStarter*>();
}

BSML_DATACACHE(settings_about) {
    return Assets::Settings::About_bsml;
}

BSML_DATACACHE(mods_idle) {
    return Assets::mods_idle_png;
}

BSML_DATACACHE(mods_selected) {
    return Assets::mods_selected_png;
}

BSML_DATACACHE(visibility) {
    return Assets::visibility_png;
}
