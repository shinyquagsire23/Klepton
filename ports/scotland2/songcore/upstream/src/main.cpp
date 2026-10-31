#include "SongCore.hpp"
#include "SongLoader/RuntimeSongLoader.hpp"
#include "SongLoader/LevelLoader.hpp"
#include "SongLoader/NavigationControllerUpdater.hpp"
#include "UI/PlayButtonsUpdater.hpp"
#include "UI/IconCache.hpp"
#include "UI/RefreshSongButton.hpp"
#include "UI/RequirementsListManager.hpp"
#include "UI/ColorsOptions.hpp"
#include "Overrides/RotationSpawnLinesOverride.hpp"
#include "UI/DeleteLevelButton.hpp"
#include "UI/RefreshSongButton.hpp"
#include "Utils/Cache.hpp"

#include "UI/ProgressBar.hpp"
#include "_config.h"
#include "config.hpp"
#include "main.hpp"
#include "lapiz/shared/zenject/Zenjector.hpp"
#include "bsml/shared/BSML.hpp"
#include "custom-types/shared/register.hpp"
#include "logging.hpp"
#include "hooking.hpp"
#include "assets.hpp"
#include "Zenject/FromBinderNonGeneric.hpp"
#include "Zenject/ConcreteBinderGeneric_1.hpp"
#include "lapiz/shared/utilities/ZenjectExtensions.hpp"
#include "lapiz/shared/AttributeRegistration.hpp"

#include "LevelSelect.hpp"
#include "Capabilities.hpp"
#include "Characteristics.hpp"
#include "PlayButtonInteractable.hpp"
#include "include/UI/ProgressBar.hpp"
#include "config.hpp"
#include <filesystem>
#include <fstream>

void RegisterDefaultCharacteristics();
void EnsureNoMedia();
void SongLoaderInstalled();

static modloader::ModInfo modInfo = {MOD_ID, VERSION, 0}; // Stores the ID and version of our mod, and is sent to the modloader upon startup

// Loads the config from disk using our modInfo, then returns it for use
// other config tools such as config-utils don't use this config, so it can be removed if those are in use
Configuration& getConfig() {
    static Configuration config(modInfo);
    return config;
}

// Called at the early stages of game loading
SONGCORE_EXPORT_FUNC void setup(CModInfo* info) {
    info->id = MOD_ID;
    info->version = VERSION;

    getConfig().Load();
    INFO("Completed setup!");
}

// Called later on in the game loading - a good time to install function hooks
SONGCORE_EXPORT_FUNC void late_load() {
    il2cpp_functions::Init();

    srand(time(nullptr));
    custom_types::Register::AutoRegister();
    BSML::Init();
    if (!LoadConfig()) SaveConfig();

    SongCore::Hooking::InstallHooks();
    auto z = Lapiz::Zenject::Zenjector::Get();

    // load cached hashes n stuff
    if (!SongCore::Utils::LoadSongInfoCache()) SongCore::Utils::SaveSongInfoCache();

    EnsureNoMedia();

    auto preferredCustomLevelPath = SongCore::API::Loading::GetPreferredCustomLevelPath();
    auto preferredCustomWIPLevelPath = SongCore::API::Loading::GetPreferredCustomWIPLevelPath();
    if (!preferredCustomLevelPath.empty() && !std::filesystem::exists(preferredCustomLevelPath)) std::filesystem::create_directories(preferredCustomLevelPath);
    if (!preferredCustomWIPLevelPath.empty() && !std::filesystem::exists(preferredCustomWIPLevelPath)) std::filesystem::create_directory(preferredCustomWIPLevelPath);

    z->Install(Lapiz::Zenject::Location::App, [](::Zenject::DiContainer* container){
        INFO("Installing RSL to location App from SongCore");
        container->BindInterfacesAndSelfTo<SongCore::Capabilities*>()->AsSingle()->NonLazy();
        container->BindInterfacesAndSelfTo<SongCore::Characteristics*>()->AsSingle()->NonLazy();
        container->BindInterfacesAndSelfTo<SongCore::PlayButtonInteractable*>()->AsSingle()->NonLazy();
        container->BindInterfacesAndSelfTo<SongCore::SongLoader::LevelLoader*>()->AsSingle()->NonLazy();
        container->BindInterfacesAndSelfTo<SongCore::SongLoader::RuntimeSongLoader*>()->AsSingle()->NonLazy();
        container->BindInterfacesAndSelfTo<SongCore::UI::IconCache*>()->AsSingle()->NonLazy();
    });

    z->Install(Lapiz::Zenject::Location::Menu, [](::Zenject::DiContainer* container) {
        container->BindInterfacesAndSelfTo<SongCore::SongLoader::NavigationControllerUpdater*>()->AsSingle()->NonLazy();
        container->BindInterfacesAndSelfTo<SongCore::UI::PlayButtonsUpdater*>()->AsSingle()->NonLazy();
        container->BindInterfacesAndSelfTo<SongCore::UI::RequirementsListManager*>()->AsSingle()->NonLazy();
        container->BindInterfacesAndSelfTo<SongCore::UI::ColorsOptions*>()->AsSingle()->NonLazy();
        container->BindInterfacesAndSelfTo<SongCore::UI::ProgressBar*>()->AsSingle()->NonLazy();
        container->BindInterfacesAndSelfTo<SongCore::UI::DeleteLevelButton*>()->AsSingle()->NonLazy();
        container->BindInterfacesAndSelfTo<SongCore::UI::RefreshSongButton*>()->AsSingle()->NonLazy();
        container->BindInterfacesAndSelfTo<SongCore::LevelSelect*>()->AsSingle()->NonLazy();
    });

    z->Install(Lapiz::Zenject::Location::GameCore, [](::Zenject::DiContainer* container) {
        container->BindInterfacesAndSelfTo<SongCore::Overrides::RotationSpawnLinesOverride*>()->AsSingle()->NonLazy();
    });


    RegisterDefaultCharacteristics();

    INFO("SongCore loaded and initialized.");
}

void RegisterDefaultCharacteristics() {
    static SafePtrUnity<GlobalNamespace::BeatmapCharacteristicSO> missingCharacteristic = SongCore::API::Characteristics::CreateCharacteristic(
        BSML::Lite::ArrayToSprite(Assets::Resources::MissingChar_png),
        "Missing Characteristic",
        "Missing Characteristic",
        "MissingCharacteristic",
        "MissingCharacteristic",
        false,
        false,
        1000
    );

    static SafePtrUnity<GlobalNamespace::BeatmapCharacteristicSO> lightshow = SongCore::API::Characteristics::CreateCharacteristic(
        BSML::Lite::ArrayToSprite(Assets::Resources::Lightshow_png),
        "Lightshow",
        "Lightshow",
        "Lightshow",
        "Lightshow",
        false,
        false,
        100
    );

    static SafePtrUnity<GlobalNamespace::BeatmapCharacteristicSO> lawless = SongCore::API::Characteristics::CreateCharacteristic(
        BSML::Lite::ArrayToSprite(Assets::Resources::Lawless_png),
        "Lawless",
        "Lawless - Anything Goes",
        "Lawless",
        "Lawless",
        false,
        false,
        101
    );

    SongCore::API::Characteristics::RegisterCustomCharacteristic(missingCharacteristic.ptr());
    SongCore::API::Characteristics::RegisterCustomCharacteristic(lightshow.ptr());
    SongCore::API::Characteristics::RegisterCustomCharacteristic(lawless.ptr());
}

void EnsureNoMedia() {
    // make sure songcore folder contains a .nomedia file to prevent icons and images absolutely decimating the images apps
    auto noMediaFilePath = std::filesystem::path("/sdcard/ModData/com.beatgames.beatsaber/Mods/SongCore/.nomedia");
    if (!std::filesystem::exists(noMediaFilePath)) {
        std::filesystem::create_directories(noMediaFilePath.parent_path());
        std::ofstream file(noMediaFilePath);
        file << '\0';
    }
}