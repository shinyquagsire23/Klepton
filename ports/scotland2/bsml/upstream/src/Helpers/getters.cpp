#include "Helpers/getters.hpp"
#include "logging.hpp"
#include "hooking.hpp"

#include "UnityEngine/Resources.hpp"
#include "UnityEngine/Transform.hpp"
#include "UnityEngine/GameObject.hpp"
#include "UnityEngine/UI/Image.hpp"
#include "GlobalNamespace/MainMenuViewController.hpp"
#include "GlobalNamespace/LevelCollectionTableView.hpp"
#include "GlobalNamespace/MainSettingsMenuViewControllersInstaller.hpp"
#include "VRUIControls/VRGraphicRaycaster.hpp"
#include "HMUI/TextSegmentedControl.hpp"
#include "HMUI/ScrollView.hpp"
#include "TMPro/TextMeshProUGUI.hpp"

using namespace TMPro;
using namespace HMUI;
using namespace UnityEngine;
using namespace VRUIControls;
using namespace Zenject;
using namespace GlobalNamespace;

#define CacheNotFoundWarningLog(type) WARNING("Can't find '{}'! (This shouldn't happen and can cause unexpected behaviour)", #type)

namespace BSML::Helpers {
    SafePtrUnity<UnityEngine::UI::Button> soloButton;
    bool TryGetSoloButton(UnityEngine::UI::Button*& button) {
        auto container = GetDiContainer();

        if(!container) {
            CacheNotFoundWarningLog(SoloButton);
            button = nullptr;
            return false;
        }

        soloButton = container->Resolve<MainMenuViewController*>()->_soloButton;
        if(!soloButton) {
            CacheNotFoundWarningLog(SoloButton);
            button = nullptr;
            return false;
        }

        button = soloButton.ptr();
        return true;
    }

    bool TryGetUITextTemplate(TMPro::TextMeshProUGUI*& textMesh) {
        UnityEngine::UI::Button* soloButton;
        if (!TryGetSoloButton(soloButton)) {
            textMesh = nullptr;
            return false;
        }
        auto transform = soloButton->transform;
        auto text = transform->Find("Text");
        textMesh = text->GetComponent<TMPro::TextMeshProUGUI*>();
        return textMesh;
    }

    SafePtr<PhysicsRaycasterWithCache> physicsRaycaster;
    PhysicsRaycasterWithCache* GetPhysicsRaycasterWithCache()
    {
        if(!physicsRaycaster) {
            physicsRaycaster = GetDiContainer()->Resolve<PhysicsRaycasterWithCache*>();
        }
        if(!physicsRaycaster) {
            CacheNotFoundWarningLog(PhysicsRaycasterWithCache);
            return nullptr;
        }
        return physicsRaycaster.ptr();
    }

    SafePtr<DiContainer> diContainer;
    DiContainer* GetDiContainer() {
        if(!diContainer) {
            ERROR("Tried getting DiContainer too early!");
            return nullptr;
        }
        return diContainer.ptr();
    }

    SafePtrUnity<HoverHintController> hoverHintController;
    HoverHintController* GetHoverHintController() {
        if(!hoverHintController)
            hoverHintController = GetDiContainer()->Resolve<HoverHintController*>();
        if(!hoverHintController) {
            CacheNotFoundWarningLog(HoverHintController);
            return nullptr;
        }
        return hoverHintController.ptr();
    }

    SafePtr<IVRPlatformHelper> platformHelper;
    IVRPlatformHelper* GetIVRPlatformHelper() {
        if (!platformHelper)
            platformHelper = GetDiContainer()->Resolve<IVRPlatformHelper*>();
        if (!platformHelper) {
            CacheNotFoundWarningLog(IVRPlatformHelper);
            return nullptr;
        }
        return platformHelper.ptr();
    }

    SafePtrUnity<TMP_FontAsset> mainTextFont;
    TMP_FontAsset* GetMainTextFont() {
        TMPro::TextMeshProUGUI* textMesh;
        if (!mainTextFont && TryGetUITextTemplate(textMesh)) {
            mainTextFont = textMesh->font;
            mainTextFont->boldSpacing = 2.2f;
        }
        if(!mainTextFont) {
            CacheNotFoundWarningLog(TMP_FontAsset);
            return nullptr;
        }
        return mainTextFont.ptr();
    }

    SafePtrUnity<Material> mainUIFontMaterial;
    Material* GetMainUIFontMaterial() {
        TMPro::TextMeshProUGUI* textMesh;
        if (!mainUIFontMaterial && TryGetUITextTemplate(textMesh)) {
            mainUIFontMaterial = textMesh->fontSharedMaterial;
        }
        if(!mainUIFontMaterial) {
            CacheNotFoundWarningLog(Material);
            return nullptr;
        }
        return mainUIFontMaterial.ptr();
    }

    SafePtrUnity<Material> noGlowUIMat;
    Material* GetUINoGlowMat() {
        if (!noGlowUIMat) {
            UnityEngine::UI::Button* soloButton;
            if (TryGetSoloButton(soloButton)) {
                noGlowUIMat = soloButton->transform->Find("Image/Image0")->GetComponent<UnityEngine::UI::Image*>()->material;
            }
        }
        if(!noGlowUIMat) {
            CacheNotFoundWarningLog(Material);
            return nullptr;
        }
        return noGlowUIMat.ptr();
    }

    SafePtrUnity<MainFlowCoordinator> mainFlowCoordinator;
    MainFlowCoordinator* GetMainFlowCoordinator() {
        if (!mainFlowCoordinator)
            mainFlowCoordinator = GetDiContainer()->Resolve<MainFlowCoordinator*>();
        if(!mainFlowCoordinator) {
            CacheNotFoundWarningLog(MainFlowCoordinator);
            return nullptr;
        }
        return mainFlowCoordinator.ptr();
    }
}

MAKE_AUTO_HOOK_MATCH(
    MainSettingsMenuViewControllersInstaller_InstallBindings,
    &GlobalNamespace::MainSettingsMenuViewControllersInstaller::InstallBindings,
    void,
    GlobalNamespace::MainSettingsMenuViewControllersInstaller* self
) {
    BSML::Helpers::diContainer = self->get_Container();
    MainSettingsMenuViewControllersInstaller_InstallBindings(self);
}
