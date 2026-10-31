#include "hooking.hpp"
#include "logging.hpp"

#include "GlobalNamespace/MainFlowCoordinator.hpp"
#include "GlobalNamespace/PlayerOptionsViewController.hpp"
#include "BSML/MenuButtons/MenuButtons.hpp"
#include "BSML/Settings/BSMLSettings.hpp"
#include "BSML/GameplaySetup/GameplaySetup.hpp"

#include "BGLib/Polyglot/Localization.hpp"

MAKE_AUTO_HOOK_MATCH(MainFlowCoordinator_DidActivate, &GlobalNamespace::MainFlowCoordinator::DidActivate, void, GlobalNamespace::MainFlowCoordinator* self, bool firstActivation, bool addedToHierarchy, bool screenSystemEnabling) {
    INFO("MainFlowCoordinator_DidActivate");
    if (firstActivation) {
        BSML::MenuButtons::get_instance()->Setup();
        BSML::BSMLSettings::get_instance()->Setup();
        BSML::GameplaySetup::get_instance()->Setup();
    }

    MainFlowCoordinator_DidActivate(self, firstActivation, addedToHierarchy, screenSystemEnabling);
}

MAKE_AUTO_HOOK_ORIG_MATCH(MainFlowCoordinator_TopViewControllerWillChange, &GlobalNamespace::MainFlowCoordinator::TopViewControllerWillChange, void, GlobalNamespace::MainFlowCoordinator* self, HMUI::ViewController* oldViewController, HMUI::ViewController* newViewController, HMUI::ViewController::AnimationType animationType)
{
	INFO("MainFlowCoordinator_TopViewControllerWillChange");
    // doesnt call orig!
    if (newViewController->Equals(self->_mainMenuViewController)) {
		self->SetLeftScreenViewController(self->_providedLeftScreenViewController, animationType);
		self->SetRightScreenViewController(self->_providedRightScreenViewController, animationType);
		self->SetBottomScreenViewController(nullptr, animationType);
	} else {
		self->SetLeftScreenViewController(nullptr, animationType);
		self->SetRightScreenViewController(nullptr, animationType);
		self->SetBottomScreenViewController(nullptr, animationType);
	}

    /*
	if (newViewController->Equals(self->howToPlayViewController))
	{
        static ConstString LABEL_HOW_TO_PLAY("LABEL_HOW_TO_PLAY");
        self->SetTitle(Polyglot::Localization::Get(LABEL_HOW_TO_PLAY), animationType);
		self->SetBottomScreenViewController(self->_playerStatisticsViewController, animationType);
        self->set_showBackButton(true);
		return;
	}
    */

	if (newViewController->Equals(self->_playerOptionsViewController)) {
        static ConstString BUTTON_PLAYER_OPTIONS("BUTTON_PLAYER_OPTIONS");
        self->SetTitle(BGLib::Polyglot::Localization::Get(BUTTON_PLAYER_OPTIONS), animationType);
        self->set_showBackButton(true);
		return;
	}

	if (newViewController->Equals(self->_optionsViewController)) {
        static ConstString LABEL_OPTIONS("LABEL_OPTIONS");
        self->SetTitle(BGLib::Polyglot::Localization::Get(LABEL_OPTIONS), animationType);
        self->set_showBackButton(true);
		return;
	}
	self->SetTitle("", animationType);
    self->set_showBackButton(false);
}
