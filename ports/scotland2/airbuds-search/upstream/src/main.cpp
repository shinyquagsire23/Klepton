#include "HMUI/FlowCoordinator.hpp"
#include "HMUI/ViewController.hpp"
#include "System/Linq/Enumerable.hpp"
#include "UnityEngine/Resources.hpp"
#include "bsml/shared/BSML.hpp"
#include "bsml/shared/BSML/ViewControllers/HotReloadViewController.hpp"
#include "bsml/shared/Helpers/getters.hpp"
#include "scotland2/shared/modloader.h"
#include <bsml/shared/Helpers/creation.hpp>

#include "BeatSaverUtils.hpp"
#include "Configuration.hpp"
#include "Log.hpp"
#include "Airbuds/AirbudsClient.hpp"
#include "UI/FlowCoordinators/AirbudsSearchFlowCoordinator.hpp"
#include "UI/GameplaySetupTab.hpp"
#include "UI/ViewControllers/SettingsViewController.hpp"
#include "main.hpp"

using AirbudsSearch::UI::FlowCoordinators::AirbudsSearchFlowCoordinator;
using namespace AirbudsSearch;

// Called at the early stages of game loading
MOD_EXTERN_FUNC void setup(CModInfo* info) noexcept {
    *info = modInfo.to_c();
    getConfig().Load();

    try {
        std::filesystem::rename("/sdcard/ModData/com.beatgames.beatsaber/logs2/airbuds-search.log", "/sdcard/ModData/com.beatgames.beatsaber/logs2/airbuds-search.1.log");
    } catch (const std::filesystem::filesystem_error& error) {
        AirbudsSearch::Log.info("Failed to rotate log: {}", error.what());
    }

    // Initialize logging. This enables saving logs to disk.
    Paper::Logger::RegisterFileContextId("airbuds-search");

    AirbudsSearch::Log.info("Version: {}", info->version);

    // visionOS has no Android logcat process. Use the existing app log.

    if (!getConfig().config.HasMember("filter")) {
        rapidjson::Value filterJson;
        filterJson.SetObject();
        getConfig().config.AddMember("filter", filterJson, getConfig().config.GetAllocator());
    }
    if (!getConfig().config["filter"].HasMember("difficulty")) {
        getConfig().config["filter"].AddMember("difficulty", "Normal", getConfig().config.GetAllocator());
    }

    auto& config = getConfig().config;
    if (!config.HasMember("airbuds")) {
        rapidjson::Value airbudsJson;
        airbudsJson.SetObject();
        airbudsJson.AddMember("refreshToken", "", config.GetAllocator());
        config.AddMember("airbuds", airbudsJson, config.GetAllocator());
    } else if (!config["airbuds"].IsObject()) {
        config["airbuds"].SetObject();
    }
    if (!config["airbuds"].HasMember("refreshToken")) {
        config["airbuds"].AddMember("refreshToken", "", config.GetAllocator());
    }

    if (config.HasMember("thirdParty") && config["thirdParty"].IsObject()) {
        const auto& thirdParty = config["thirdParty"];
        if (config["airbuds"]["refreshToken"].IsString()
            && std::string(config["airbuds"]["refreshToken"].GetString()).empty()
            && thirdParty.HasMember("refreshToken")
            && thirdParty["refreshToken"].IsString()) {
            config["airbuds"]["refreshToken"].SetString(thirdParty["refreshToken"].GetString(), config.GetAllocator());
        }
        config.RemoveMember("thirdParty");
    }

    if (config.HasMember("requirePassword")) {
        config.RemoveMember("requirePassword");
    }

    getConfig().Write();
}

SC2_RECORD_SEARCH_HOOK(onDismissFlowCoordinator, &HMUI::FlowCoordinator::DismissFlowCoordinator)
MAKE_HOOK_MATCH(
    onDismissFlowCoordinator,
    &HMUI::FlowCoordinator::DismissFlowCoordinator,
    void,
    HMUI::FlowCoordinator* self,
    HMUI::FlowCoordinator* flowCoordinator,
    HMUI::ViewController::AnimationDirection animationDirection,
    System::Action* finishedCallback,
    bool immediately) {
    AirbudsSearch::Log.info(
        "onDismissFlowController: return = {} self = {} ({}) fc = {} ({})",
        AirbudsSearch::returnToAirbudsSearch,
        static_cast<void*>(self),
        self->get_gameObject()->get_name(),
        static_cast<void*>(flowCoordinator),
        flowCoordinator->get_gameObject()->get_name());

    // Call the original function
    onDismissFlowCoordinator(self, flowCoordinator, animationDirection, finishedCallback, true);

    // Return to the Airbuds Search flow coordinator
    if (AirbudsSearch::returnToAirbudsSearch) {
        auto currentFlowCoordinator = BSML::Helpers::GetMainFlowCoordinator()->YoungestChildFlowCoordinatorOrSelf();
        UnityW<AirbudsSearch::UI::FlowCoordinators::AirbudsSearchFlowCoordinator> airbudsSearchFlowCoordinator = UnityEngine::Resources::FindObjectsOfTypeAll<AirbudsSearch::UI::FlowCoordinators::AirbudsSearchFlowCoordinator*>()->FirstOrDefault();
        if (airbudsSearchFlowCoordinator) {
            currentFlowCoordinator->PresentFlowCoordinator(airbudsSearchFlowCoordinator, nullptr, HMUI::ViewController::AnimationDirection::Horizontal, true, false);
        }
    }
};

void AirbudsSearch::openAirbudsSearchFlowCoordinator() {
    // Create the main flow coordinator
    if (!AirbudsSearch::airbudsSearchFlowCoordinator_) {
        AirbudsSearch::airbudsSearchFlowCoordinator_ = BSML::Helpers::CreateFlowCoordinator<AirbudsSearch::UI::FlowCoordinators::AirbudsSearchFlowCoordinator*>();
    }

    HMUI::FlowCoordinator* parentFlow = BSML::Helpers::GetMainFlowCoordinator()->YoungestChildFlowCoordinatorOrSelf();

    if (parentFlow) {
        parentFlow->PresentFlowCoordinator(AirbudsSearch::airbudsSearchFlowCoordinator_.ptr(), nullptr, HMUI::ViewController::AnimationDirection::Horizontal, false, false);
    }
}

// Called later on in the game loading - a good time to install function hooks
MOD_EXTERN_FUNC void late_load() noexcept {
    il2cpp_functions::Init();

    // make sure this is after il2cpp_functions::Init()
    BSML::Init();

    // Register custom types
    custom_types::Register::AutoRegister();

    // Register main menu button
    BSML::Register::RegisterMenuButton("Airbuds Search", "Search your Airbuds history", []() {
        AirbudsSearch::Log.info("Menu button clicked");
        AirbudsSearch::openAirbudsSearchFlowCoordinator();
    });

    // Register GameplaySetup Tab (left side of song selection screen)
    AirbudsSearch::UI::GameplaySetupTab::Register();

    // Register settings page
    BSML::Register::RegisterSettingsMenu<AirbudsSearch::UI::ViewControllers::SettingsViewController*>("Airbuds Search", false);

    // Install hooks
    INSTALL_HOOK(AirbudsSearch::Log, onDismissFlowCoordinator);

    // Initialize the Airbuds client
    if (!AirbudsSearch::airbudsClient) {
        AirbudsSearch::airbudsClient = std::make_shared<airbuds::Client>();
    }

    // Initialize BeatSaver utils
    AirbudsSearch::BeatSaverUtils::getInstance().init();
}
