#include "_config.h"
#include "main.hpp"
#include "utilities/hooking.hpp"
#include "utilities/logging.hpp"
#include "utilities/MainThreadScheduler.hpp"

#include "zenject/Zenjector.hpp"
#include "zenject/Location.hpp"
#include "Zenject/FromBinderNonGeneric.hpp"

#include "installers/LapizGameplayInstaller.hpp"
#include "installers/LapizGameCoreInstaller.hpp"
#include "utilities/logging.hpp"

static modloader::ModInfo modInfo{MOD_ID, VERSION, 0}; // Stores the ID and version of our mod, and is sent to the modloader upon startup

// Called at the early stages of game loading
LAPIZ_EXPORT_FUNC void setup(CModInfo* info) {
    info->id = MOD_ID;
    info->version = VERSION;
    info->version_long = 0;

    INFO(MOD_ID " v" VERSION " completed setup!");
}

bool loaded = false;
// Called later on in the game loading - a good time to install function hooks
LAPIZ_EXPORT_FUNC void load() {
    if (loaded) return;
    loaded = true;
    il2cpp_functions::Init();
    custom_types::Register::AutoRegister();

    INFO("Installing Zenject bindings and hooks..");

    Hooks::InstallHooks();
    using namespace Lapiz::Zenject;
    auto zenjector = Zenjector::Get();
    zenjector->Install(Location::App, [](::Zenject::DiContainer* container){ container->BindInterfacesAndSelfTo<Lapiz::Utilities::MainThreadScheduler*>()->AsSingle(); });
    zenjector->Install<Lapiz::Installers::LapizGameplayInstaller*>(Location::Player | Location::Tutorial);
    zenjector->Install<Lapiz::Installers::LapizGameCoreInstaller*>(Location::GameCore);
}
