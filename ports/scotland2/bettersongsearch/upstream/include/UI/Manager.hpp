#pragma once

#include "bsml/shared/BSML.hpp"
#include "bsml/shared/Helpers/creation.hpp"
#include "HMUI/FlowCoordinator.hpp"
#include "HMUI/NoTransitionsButton.hpp"
#include "HMUI/ViewController.hpp"
#include "UI/FlowCoordinators/BetterSongSearchFlowCoordinator.hpp"
#include "UnityEngine/UI/Button.hpp"

namespace BetterSongSearch::UI {
    class Manager {
        HMUI::FlowCoordinator* parentFlow;
        SafePtrUnity<BetterSongSearch::UI::FlowCoordinators::BetterSongSearchFlowCoordinator> flow;
        BSML::MenuButton* menuButton;

       public:
        Manager(Manager const&) = delete;  // no accidental copying
        Manager() = default;

        void Init();

        custom_types::Helpers::Coroutine Debug();

        void ShowFlow(bool immediately);

        void Close(bool immediately = false, bool downloadAbortConfim = true);

        void GoToSongSelect();
        void DestroyFlow();
    };

    inline static Manager manager;
}  // namespace BetterSongSearch::UI
