#include "BSML/FlowCoordinators/MainMenuHolderFlowCoordinator.hpp"
#include "BSML.hpp"

#include "custom-types/shared/delegate.hpp"
#include "logging.hpp"
#include "UnityEngine/WaitForEndOfFrame.hpp"
#include "Helpers/creation.hpp"
#include "Helpers/getters.hpp"

DEFINE_TYPE(BSML, MainMenuHolderFlowCoordinator);

namespace BSML {
    SafePtrUnity<MainMenuHolderFlowCoordinator> MainMenuHolderFlowCoordinator::instance;
    MainMenuHolderFlowCoordinator* MainMenuHolderFlowCoordinator::get_instance() {
        if (!instance) {
            instance = BSML::Helpers::CreateFlowCoordinator<MainMenuHolderFlowCoordinator*>();
        }
        return instance.ptr();
    }

    void MainMenuHolderFlowCoordinator::DidActivate(bool firstActivation, bool addedToHierarchy, bool screenSystemEnabling) {
        if (firstActivation) {
            showBackButton = true;
            SetTitle("PlaceHolder", HMUI::ViewController::AnimationType::Out);
            placeholder = BSML::Helpers::CreateViewController();
            ProvideInitialViewControllers(placeholder, nullptr, nullptr, nullptr, nullptr);
        }

        StartCoroutine(custom_types::Helpers::CoroutineHelper::New(EndOfFramePresentVC()));
    }

    void MainMenuHolderFlowCoordinator::BackButtonWasPressed(HMUI::ViewController* topViewController) {
        DEBUG("Back Button was pressed");
        _parentFlowCoordinator->DismissFlowCoordinator(this, HMUI::ViewController::AnimationDirection::Horizontal, nullptr, false);
    }

    custom_types::Helpers::Coroutine MainMenuHolderFlowCoordinator::EndOfFramePresentVC() {
        co_yield reinterpret_cast<System::Collections::IEnumerator*>(UnityEngine::WaitForEndOfFrame::New_ctor());

        if (currentRegistration) {
            SetTitle(currentRegistration->title, HMUI::ViewController::AnimationType::Out);
            if (!_mainScreenViewControllers->Contains(currentRegistration->viewController)) {
                ReplaceTopViewController(currentRegistration->viewController, this, this, nullptr, HMUI::ViewController::AnimationType::In, HMUI::ViewController::AnimationDirection::Horizontal);
            } else {
                currentRegistration->viewController->__Activate(false, false);
            }
        }

        co_return;
    }

#pragma region mainMenuregistration
    std::vector<MainMenuRegistration*> MainMenuRegistration::registrations;
    MainMenuRegistration* MainMenuRegistration::get_registration(const std::string_view& buttonText) {
        for (auto reg : registrations)
            if (reg->buttonText == buttonText) return reg;
        return nullptr;
    }

    MainMenuRegistration::MainMenuRegistration(const std::string_view& title, const std::string_view& buttonText, const std::string_view& hoverHint, const System::Type* csType, const MenuSource registrationType) :
        title(title),
        buttonText(buttonText),
        hoverHint(hoverHint),
        csType(csType),
        setupFunc(nullptr),
        registrationType(registrationType),
        viewController(nullptr) {
        BSML::Events::onGameDidRestart += {&MainMenuRegistration::OnGameDidRestart, this};
    }

    MainMenuRegistration::MainMenuRegistration(const std::string_view& title, const std::string_view& buttonText, const std::string_view& hoverHint, const std::function<void(HMUI::ViewController*, bool, bool, bool)> setupFunc) :
        title(title),
        buttonText(buttonText),
        hoverHint(hoverHint),
        csType(csTypeOf(HMUI::ViewController*)),
        setupFunc(setupFunc),
        registrationType(MenuSource::Method),
        viewController(nullptr) {
        BSML::Events::onGameDidRestart += {&MainMenuRegistration::OnGameDidRestart, this};
    }

    MainMenuRegistration::~MainMenuRegistration() {
        BSML::Events::onGameDidRestart -= {&MainMenuRegistration::OnGameDidRestart, this};
    }

    void MainMenuRegistration::OnGameDidRestart() {
        if (viewController && viewController->m_CachedPtr.m_value) {
            UnityEngine::Object::DestroyImmediate(viewController);
        }

        viewController = nullptr;
    }

    void MainMenuRegistration::Present() {
        auto presentOn = BSML::Helpers::GetMainFlowCoordinator()->YoungestChildFlowCoordinatorOrSelf();
        switch (registrationType) {
            case MenuSource::FlowCoordinator:
                PresentWithFlowCoordinator(presentOn);
                break;
            case MenuSource::ViewController:
                PresentWithViewController(presentOn);
                break;
            case MenuSource::Method:
                PresentWithMethod(presentOn);
                break;
            case MenuSource::BSMLContent: [[fallthrough]];
            case MenuSource::Component:
                // TODO: should this be supported somehow?
                throw std::runtime_error("Invalid menu source in main menu registration!");
        }
    }

    void MainMenuRegistration::PresentWithFlowCoordinator(HMUI::FlowCoordinator* presentOn) {
        if (!flowCoordinator || !flowCoordinator->m_CachedPtr.m_value) {
            flowCoordinator = BSML::Helpers::CreateFlowCoordinator(const_cast<System::Type*>(csType));
        }

        presentOn->PresentFlowCoordinator(flowCoordinator, nullptr, HMUI::ViewController::AnimationDirection::Horizontal, false, false);
    }

    void MainMenuRegistration::PresentWithViewController(HMUI::FlowCoordinator* presentOn) {
        if (!viewController || !viewController->m_CachedPtr.m_value) {
            viewController = BSML::Helpers::CreateViewController(const_cast<System::Type*>(csType));
        }

        auto fc = MainMenuHolderFlowCoordinator::get_instance();
        fc->currentRegistration = this;

        presentOn->PresentFlowCoordinator(fc, nullptr, HMUI::ViewController::AnimationDirection::Horizontal, true, false);
    }

    void MainMenuRegistration::PresentWithMethod(HMUI::FlowCoordinator* presentOn) {
        if (!viewController || !viewController->m_CachedPtr.m_value) {
            viewController = BSML::Helpers::CreateViewController(const_cast<System::Type*>(csType));
            viewController->add_didActivateEvent(custom_types::MakeDelegate<HMUI::ViewController::DidActivateDelegate*>(viewController, setupFunc));
        }

        auto fc = MainMenuHolderFlowCoordinator::get_instance();
        fc->currentRegistration = this;

        presentOn->PresentFlowCoordinator(fc, nullptr, HMUI::ViewController::AnimationDirection::Horizontal, true, false);
    }
#pragma endregion
}
