#include "UI/FlowCoordinators/AirbudsSearchFlowCoordinator.hpp"

#include "GlobalNamespace/SongPreviewPlayer.hpp"
#include "UnityEngine/ScriptableObject.hpp"
#include "bsml/shared/Helpers/getters.hpp"
#include <bsml/shared/Helpers/creation.hpp>

#include "Configuration.hpp"
#include "Log.hpp"
#include "main.hpp"

DEFINE_TYPE(AirbudsSearch::UI::FlowCoordinators, AirbudsSearchFlowCoordinator);

using namespace AirbudsSearch::UI::FlowCoordinators;

void AirbudsSearchFlowCoordinator::DidActivate(bool isFirstActivation, bool addedToHierarchy, bool screenSystemEnabling) {
    AirbudsSearch::Log.info("activate");
    if (!isFirstActivation) {
        return;
    }

    // Set title
    SetTitle("Airbuds Search", HMUI::ViewController::AnimationType::In);

    // Make sure the back button is visible
    showBackButton = true;

    // Show view controllers
    if (!AirbudsSearch::hasAirbudsRefreshToken()) {
        settingsViewController_ = BSML::Helpers::CreateViewController<ViewControllers::SettingsViewController*>();
        ProvideInitialViewControllers(settingsViewController_, nullptr, nullptr, nullptr, nullptr);
        return;
    }

    // Initialize view controllers
    mainViewController_ = BSML::Helpers::CreateViewController<ViewControllers::MainViewController*>();
    filterOptionsViewController_ = BSML::Helpers::CreateViewController<ViewControllers::FilterOptionsViewController*>();
    downloadHistoryViewController_ = BSML::Helpers::CreateViewController<ViewControllers::DownloadHistoryViewController*>();

    ProvideInitialViewControllers(mainViewController_, filterOptionsViewController_, downloadHistoryViewController_, nullptr, nullptr);
}

void AirbudsSearchFlowCoordinator::BackButtonWasPressed(HMUI::ViewController* topViewController) {
    AirbudsSearch::Log.info("back button pressed, set return = false");
    AirbudsSearch::returnToAirbudsSearch = false;

    // Stop the song preview if it's playing
    UnityW<GlobalNamespace::SongPreviewPlayer> songPreviewPlayer = BSML::Helpers::GetDiContainer()->Resolve<GlobalNamespace::SongPreviewPlayer*>();
    if (songPreviewPlayer) {
        songPreviewPlayer->CrossfadeToDefault();
    }

    // Dismiss this flow coordinator
    this->_parentFlowCoordinator->DismissFlowCoordinator(this, HMUI::ViewController::AnimationDirection::Horizontal, nullptr, false);
}

void AirbudsSearchFlowCoordinator::reset() {
    if (!airbudsSearchFlowCoordinator_) {
        return;
    }
    auto p = airbudsSearchFlowCoordinator_->_parentFlowCoordinator;
    AirbudsSearch::Log.info("p = {}", static_cast<void*>(p));
    if (p) {
        p->DismissFlowCoordinator(airbudsSearchFlowCoordinator_.ptr(), HMUI::ViewController::AnimationDirection::Horizontal, nullptr, true);
    }
    AirbudsSearch::airbudsSearchFlowCoordinator_ = nullptr;
}

void AirbudsSearchFlowCoordinator::reopen() {
    AirbudsSearchFlowCoordinator::reset();
    AirbudsSearch::openAirbudsSearchFlowCoordinator();
}
