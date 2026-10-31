#include "BSML/FloatingScreen/FloatingScreen.hpp"
#include "BSML/FloatingScreen/FloatingScreenHandle.hpp"
#include "Helpers/creation.hpp"
#include "Helpers/getters.hpp"
#include "Helpers/utilities.hpp"

#include "HMUI/CurvedCanvasSettings.hpp"
#include "HMUI/ImageView.hpp"
#include "UnityEngine/Resources.hpp"
#include "UnityEngine/PrimitiveType.hpp"
#include "UnityEngine/Canvas.hpp"
#include "UnityEngine/UI/Image.hpp"
#include "UnityEngine/AdditionalCanvasShaderChannels.hpp"
#include "UnityEngine/UI/CanvasScaler.hpp"
#include "UnityEngine/UI/RectMask2D.hpp"
#include "VRUIControls/VRGraphicRaycaster.hpp"


DEFINE_TYPE(BSML, FloatingScreen);

static inline UnityEngine::AdditionalCanvasShaderChannels operator |(UnityEngine::AdditionalCanvasShaderChannels a, UnityEngine::AdditionalCanvasShaderChannels b) {
    return UnityEngine::AdditionalCanvasShaderChannels(a.value__ | b.value__);
}

namespace BSML {
    SafePtrUnity<UnityEngine::Material> FloatingScreen::_fogMaterial;
    FloatingScreen* FloatingScreen::CreateFloatingScreen(UnityEngine::Vector2 screenSize, bool createHandle, UnityEngine::Vector3 position, UnityEngine::Quaternion rotation, float curvatureRadius, bool hasBackground) {
        auto components = ArrayW<System::Type*>(static_cast<il2cpp_array_size_t>(5));
        components[0] = csTypeOf(BSML::FloatingScreen*);
        components[1] = csTypeOf(UnityEngine::UI::CanvasScaler*);
        components[2] = csTypeOf(UnityEngine::UI::RectMask2D*);
        components[3] = csTypeOf(VRUIControls::VRGraphicRaycaster*);
        components[4] = csTypeOf(HMUI::CurvedCanvasSettings*);

        auto screenGO = UnityEngine::GameObject::New_ctor("BSMLFloatingScreen", components);
        screenGO->set_layer(5);
        auto screen = screenGO->GetComponent<FloatingScreen*>();

        screen->GetComponent<VRUIControls::VRGraphicRaycaster*>()->_physicsRaycaster = Helpers::GetPhysicsRaycasterWithCache();

        auto curvedCanvasSettings = screen->GetComponent<HMUI::CurvedCanvasSettings*>();
        curvedCanvasSettings->SetRadius(curvatureRadius);

        auto canvas = screen->GetComponent<UnityEngine::Canvas*>();
        canvas->set_additionalShaderChannels(UnityEngine::AdditionalCanvasShaderChannels::TexCoord1 | UnityEngine::AdditionalCanvasShaderChannels::TexCoord2);
        canvas->set_sortingOrder(4);

        auto scaler = screen->GetComponent<UnityEngine::UI::CanvasScaler*>();
        scaler->set_dynamicPixelsPerUnit(3.44f);
        scaler->set_referencePixelsPerUnit(10.0f);

        if (hasBackground)
        {
            auto components = ArrayW<System::Type*>(static_cast<il2cpp_array_size_t>(2));
            components[0] = csTypeOf(UnityEngine::RectTransform*);
            components[1] = csTypeOf(HMUI::ImageView*);

            auto backGroundGo = UnityEngine::GameObject::New_ctor("bg", components);
            backGroundGo->set_layer(5);
            backGroundGo->get_transform()->SetParent(canvas->get_transform(), false);
            auto rectTransform = backGroundGo->GetComponent<UnityEngine::RectTransform*>();
            rectTransform->set_sizeDelta(screenSize);
            rectTransform->set_anchorMin({0, 0});
            rectTransform->set_anchorMax({1, 1});
            rectTransform->set_offsetMin({0, 0});
            rectTransform->set_offsetMax({0, 0});

            auto background = backGroundGo->GetComponent<HMUI::ImageView*>();
            background->set_sprite(::BSML::Utilities::FindSpriteCached("MainScreenMask"));
            background->set_type(UnityEngine::UI::Image::Type::Sliced);
            background->set_color({0.7450981f, 0.7450981f, 0.7450981f, 1.0f});
            if (!_fogMaterial)
                _fogMaterial = UnityEngine::Resources::FindObjectsOfTypeAll<UnityEngine::Material*>()->FirstOrDefault([](auto x){ return x->get_name() == "UIFogBG"; });
            background->set_material(_fogMaterial.ptr());
            background->set_preserveAspect(true);
        }

        auto screenTransform = screen->get_transform();
        screenTransform->set_position(position);
        screenTransform->set_rotation(rotation);
        screenTransform->set_localScale({0.02f, 0.02f, 0.02f});

        screen->set_ScreenSize(screenSize);
        screen->set_ShowHandle(createHandle);

        screen->get_gameObject()->set_layer(5);

        return screen;
    }

    FloatingScreen* FloatingScreen::CreateFloatingScreenWithViewcontroller(Il2CppReflectionType* vcType, UnityEngine::Vector2 screenSize, bool createHandle, UnityEngine::Vector3 position, UnityEngine::Quaternion rotation, float curvatureRadius, bool hasBackground) {
        auto scr = CreateFloatingScreen(screenSize, createHandle, position, rotation, curvatureRadius, hasBackground);
        auto vc = BSML::Helpers::CreateViewController(reinterpret_cast<System::Type*>(vcType));
        scr->SetRootViewController(vc, HMUI::ViewController::AnimationType::In);
        return scr;
    }

    void FloatingScreen::CreateHandle(VRUIControls::VRPointer* pointer) {
        if (!(handle && handle->m_CachedPtr.m_value)) {
            handle = UnityEngine::GameObject::CreatePrimitive(UnityEngine::PrimitiveType::Cube);
            handle->get_transform()->SetParent(get_transform());
            handle->get_transform()->set_localRotation(UnityEngine::Quaternion::get_identity());
            handle->set_layer(5);
            UpdateHandle();

            // Add the handle component
            auto screenhandle = handle->AddComponent<FloatingScreenHandle*>();
            screenhandle->set_enabled(true);
            screenhandle->Init(this);
        }

    }

    void FloatingScreen::OnHandleGrab(VRUIControls::VRPointer* pointer) {
        if (HandleGrabbed.size() > 0) HandleGrabbed.invoke(this, FloatingScreenHandleEventArgs(pointer, get_transform()->get_position(), get_transform()->get_rotation()));
    }

    void FloatingScreen::OnHandleReleased(VRUIControls::VRPointer* pointer) {
        if (HandleReleased.size() > 0) HandleReleased.invoke(this, FloatingScreenHandleEventArgs(pointer, get_transform()->get_position(), get_transform()->get_rotation()));
    }

    void FloatingScreen::UpdateHandle() {
        if (!handle || !handle->m_CachedPtr.m_value) return;
        auto screenSize = get_ScreenSize();
        switch (get_HandleSide())
        {
            case Side::Left:
                handle->get_transform()->set_localPosition({-screenSize.x / 2.0f, 0.0f, 0.0f});
                handle->get_transform()->set_localScale({screenSize.x / 15.0f, screenSize.y * 0.8f, screenSize.x / 15.0f});
                break;
            case Side::Right:
                handle->get_transform()->set_localPosition({screenSize.x / 2.0f, 0.0f, 0.0f});
                handle->get_transform()->set_localScale({screenSize.x / 15.0f, screenSize.y * 0.8f, screenSize.x / 15.0f});
                break;
            case Side::Top:
                handle->get_transform()->set_localPosition({0.0f, screenSize.y / 2.0f, 0.0f});
                handle->get_transform()->set_localScale({screenSize.x * 0.8f, screenSize.y / 15.0f, screenSize.y / 15.0f});
                break;
            case Side::Bottom:
                handle->get_transform()->set_localPosition({0.0f, -screenSize.y / 2.0f, 0.0f});
                handle->get_transform()->set_localScale({screenSize.x * 0.8f, screenSize.y / 15.0f, screenSize.y / 15.0f});
                break;
            case Side::Full:
                handle->get_transform()->set_localPosition({});
                handle->get_transform()->set_localScale({screenSize.x, screenSize.y, screenSize.x / 15.0f});
                break;
        }

        handle->GetComponent<UnityEngine::MeshRenderer*>()->set_enabled(get_HandleSide() != Side::Full);
    }

    void FloatingScreen::OnDestroy() {
        _isBeingDestroyed = true;
    }


    UnityEngine::RectTransform* FloatingScreen::get_rectTransform() {
        return transform.cast<UnityEngine::RectTransform>();
    }

    UnityEngine::Vector2 FloatingScreen::get_ScreenSize() {
        return get_rectTransform()->get_sizeDelta();
    }

    void FloatingScreen::set_ScreenSize(UnityEngine::Vector2 value) {
        get_rectTransform()->set_sizeDelta(value);
        UpdateHandle();
    }

    UnityEngine::Vector3 FloatingScreen::get_ScreenPosition() {
        return get_transform()->get_position();
    }

    void FloatingScreen::set_ScreenPosition(UnityEngine::Vector3 value) {
        get_transform()->set_position(value);
    }

    UnityEngine::Quaternion FloatingScreen::get_ScreenRotation() {
        return get_transform()->get_rotation();
    }

    void FloatingScreen::set_ScreenRotation(UnityEngine::Quaternion value) {
        get_transform()->set_rotation(value);
    }

    bool FloatingScreen::get_ShowHandle() {
        return _showHandle;
    }

    void FloatingScreen::set_ShowHandle(bool value) {
        _showHandle = value;
        if (_showHandle) {
            if (!handle || !handle->m_CachedPtr.m_value) CreateHandle();
            else handle->SetActive(true);
        } else if (!_showHandle && handle && handle->m_CachedPtr.m_value) {
            handle->SetActive(false);
        }
    }

    bool FloatingScreen::get_HighlightHandle() {
        return _highlightHandle;
    }

    void FloatingScreen::set_HighlightHandle(bool value) {
        _highlightHandle = value;
    }

    Side FloatingScreen::get_HandleSide() {
        return _handleSide;
    }

    void FloatingScreen::set_HandleSide(Side value) {
        _handleSide = value;
        UpdateHandle();
    }

}
