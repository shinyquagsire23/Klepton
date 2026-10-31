#include "BSML/MenuButtons/MenuButtons.hpp"
#include "logging.hpp"

#include "UnityEngine/WaitForSeconds.hpp"
#include "UnityEngine/GameObject.hpp"
#include "UnityEngine/Resources.hpp"
#include "HMUI/Screen.hpp"
#include "HMUI/ModalView.hpp"

#include "BSML/SharedCoroutineStarter.hpp"

#include "Helpers/creation.hpp"
#include "Helpers/getters.hpp"
#include "Helpers/delegates.hpp"

DEFINE_TYPE(BSML, MenuButtons)

namespace BSML {
    SafePtr<MenuButtons> MenuButtons::instance;
    MenuButtons* MenuButtons::get_instance() {
        if (!instance) {
            instance.emplace(MenuButtons::New_ctor());
        }
        return instance.ptr();
    }
    ListW<System::Object*> MenuButtons::get_buttons() {
        if (!_buttons) {
            _buttons = ListW<System::Object*>::New();
        }
        return _buttons;
    }

    bool MenuButtons::Registerbutton(MenuButton* button) {
        if (!button) return false;
        auto btns = get_buttons();
        auto btnItr = std::find_if(btns.begin(), btns.end(), [button](auto b){ return b && (reinterpret_cast<MenuButton*>(b)->text == button->text); });
        if (btnItr != btns.end()) {
            ERROR("can't register a button with the same text ('{}') as an existing one!", button->text);
            return false;
        }
        btns->Add(button);
        Refresh();
        return true;
    }

    bool MenuButtons::UnRegisterbutton(MenuButton* button) {
        if (!button) return false;
        auto btns = get_buttons();
        bool result = btns->Remove(button);
        Refresh();
        return result;
    }

    void MenuButtons::Setup() {
        DEBUG("Setup");
        if (!menuButtonsViewController || !menuButtonsViewController->m_CachedPtr.m_value) {
            DEBUG("Making View Controller");
            menuButtonsViewController = Helpers::CreateViewController<MenuButtonsViewController*>();
            menuButtonsViewController->buttons = get_buttons();
        }

        auto coroStarter = BSML::SharedCoroutineStarter::get_instance();
        if (presentViewCoroutine && presentViewCoroutine->m_Ptr.m_value) {
            coroStarter->StopCoroutine(presentViewCoroutine);
        }

        presentViewCoroutine = coroStarter->StartCoroutine(custom_types::Helpers::CoroutineHelper::New(PresentView()));
    }

    void MenuButtons::Refresh() {
        if (!menuButtonsViewController || !menuButtonsViewController->m_CachedPtr.m_value) return;
        menuButtonsViewController->RefreshView();
    }

    void MenuButtons::ShowView(bool firstActivation, bool addedToHierarchy, bool screenSystemEnabling) {
        if (!leftScreen || !leftScreen->m_CachedPtr.m_value) {
            leftScreen = UnityEngine::Resources::FindObjectsOfTypeAll<HMUI::Screen*>()->FirstOrDefault([](auto x){ return x->get_gameObject()->get_name() == "LeftScreen"; });
        }

        auto modals = leftScreen->GetComponentsInChildren<HMUI::ModalView*>();
        for (auto m : modals) {
            m->OnDisable();
        }
        auto mainFc = BSML::Helpers::GetMainFlowCoordinator();
        mainFc->_providedLeftScreenViewController = menuButtonsViewController;
        mainFc->SetLeftScreenViewController(menuButtonsViewController, HMUI::ViewController::AnimationType::None);
    }

    custom_types::Helpers::Coroutine MenuButtons::PresentView() {
        DEBUG("Presenting view");
        co_yield reinterpret_cast<System::Collections::IEnumerator*>(UnityEngine::WaitForSeconds::New_ctor(0.2f));
        GlobalNamespace::MainFlowCoordinator* mainFlowCoordinator = BSML::Helpers::GetMainFlowCoordinator();

        ShowView(false, false, false);
        auto vc = UnityEngine::Resources::FindObjectsOfTypeAll<GlobalNamespace::MainMenuViewController*>()->FirstOrDefault();
        std::function<void(bool, bool, bool)> fun = std::bind(&MenuButtons::ShowView, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3);
        vc->add_didActivateEvent(custom_types::MakeDelegate<HMUI::ViewController::DidActivateDelegate*>(fun));

        presentViewCoroutine = nullptr;
        co_return;
    }
}
