#pragma once

#include <custom-types/shared/macros.hpp>
#include <UnityEngine/GameObject.hpp>

namespace AirbudsSearch::UI {

// GameplaySetup Tab manager for Airbuds Search
// Clicking the tab directly opens the FlowCoordinator
class GameplaySetupTab {
public:
    // Register the tab with BSML GameplaySetup
    static void Register();

private:
    // Callback when the tab is activated - directly open FlowCoordinator
    static void OnDidActivate(UnityEngine::GameObject* parent, bool firstActivation);

    static bool isRegistered_;
};

}
