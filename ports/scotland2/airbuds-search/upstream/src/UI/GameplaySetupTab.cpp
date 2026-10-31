#include "UI/GameplaySetupTab.hpp"

#include <bsml/shared/BSML.hpp>
#include <bsml/shared/BSML/GameplaySetup/GameplaySetup.hpp>
#include <bsml/shared/BSML-Lite/Creation/Buttons.hpp>
#include <bsml/shared/BSML-Lite/Creation/Layout.hpp>
#include <UnityEngine/Vector2.hpp>
#include <UnityEngine/UI/ContentSizeFitter.hpp>

#include "Log.hpp"
#include "main.hpp"

namespace AirbudsSearch::UI {

bool GameplaySetupTab::isRegistered_ = false;

void GameplaySetupTab::Register() {
    if (isRegistered_) {
        return;
    }

    AirbudsSearch::Log.info("Registering GameplaySetup Tab");

    const bool result = BSML::GameplaySetup::get_instance()->AddTab(
        &OnDidActivate,
        "Airbuds",
        BSML::MenuType::All
    );

    if (result) {
        isRegistered_ = true;
        AirbudsSearch::Log.info("GameplaySetup Tab registered successfully");
    } else {
        AirbudsSearch::Log.error("Failed to register GameplaySetup Tab");
    }
}

void GameplaySetupTab::OnDidActivate(UnityEngine::GameObject* parent, bool firstActivation) {
    if (!firstActivation) {
        return;
    }

    AirbudsSearch::Log.info("GameplaySetup Tab activated");

    // Create a simple centered button
    auto* container = BSML::Lite::CreateVerticalLayoutGroup(parent->get_transform());
    container->set_childAlignment(UnityEngine::TextAnchor::MiddleCenter);
    container->set_childControlHeight(false);
    container->set_childForceExpandHeight(false);
    container->set_childControlWidth(false);
    container->set_childForceExpandWidth(false);

    auto* fitter = container->get_gameObject()->AddComponent<UnityEngine::UI::ContentSizeFitter*>();
    fitter->set_horizontalFit(UnityEngine::UI::ContentSizeFitter::FitMode::PreferredSize);
    fitter->set_verticalFit(UnityEngine::UI::ContentSizeFitter::FitMode::PreferredSize);

    // Single large button
    BSML::Lite::CreateUIButton(
        container->get_transform(),
        "Open Airbuds Search",
        UnityEngine::Vector2{0, 0},
        UnityEngine::Vector2{50, 12},
        []() {
            AirbudsSearch::Log.info("GameplaySetup Tab: Open button clicked");
            AirbudsSearch::openAirbudsSearchFlowCoordinator();
        }
    );
}

}
