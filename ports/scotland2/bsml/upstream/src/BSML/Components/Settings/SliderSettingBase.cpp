#include "BSML/Components/Settings/SliderSettingBase.hpp"
#include "Helpers/utilities.hpp"

#include "UnityEngine/UI/Image.hpp"
#include "UnityEngine/Object.hpp"
#include "UnityEngine/GameObject.hpp"
#include "UnityEngine/Transform.hpp"
#include "UnityEngine/RectTransform.hpp"
#include "UnityEngine/Vector2.hpp"
#include "UnityEngine/UI/Selectable.hpp"

DEFINE_TYPE(BSML, SliderSettingBase);

namespace BSML {
    void SliderSettingBase::construct() {
        genericSetting = GenericSettingWrapper::New_ctor();
    }

    bool SliderSettingBase::get_interactable() {
        return slider ? static_cast<UnityEngine::UI::Selectable*>(slider)->get_interactable() : false;
    }

    void SliderSettingBase::set_interactable(bool value) {
        if (slider) {
            static_cast<UnityEngine::UI::Selectable*>(slider)->set_interactable(value);

            if (showButtons) {
                set_enableInc(value);
                set_enableDec(value);
            }
        }
    }

    void SliderSettingBase::set_enableDec(bool value) {
        if (decButton) {
            decButton->set_interactable(value);
        }
    }

    void SliderSettingBase::set_enableInc(bool value) {
        if (incButton) {
            incButton->set_interactable(value);
        }
    }

    void SliderSettingBase::BaseSetup() {
        incButton = slider->_incButton;
        decButton = slider->_decButton;

        if (!showButtons) {
            slider->get_image()->set_sprite(Utilities::FindSpriteCached("RoundRect10"));
            UnityEngine::Object::Destroy(incButton->get_gameObject());
            UnityEngine::Object::Destroy(decButton->get_gameObject());
            auto transform = slider->transform.cast<UnityEngine::RectTransform>();
            transform->set_sizeDelta({38, 0});
            auto bgRect = transform->Find("BG").cast<UnityEngine::RectTransform>();
            bgRect->set_sizeDelta({0, 6});
            auto slidingAreaRect = transform->Find("SlidingArea").cast<UnityEngine::RectTransform>();
            slidingAreaRect->set_sizeDelta({-4, -4});
        }
    }
}
