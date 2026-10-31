#include "BSML/Tags/ModalColorPickerTag.hpp"
#include "BSML/Components/ModalColorPicker.hpp"
#include "BSML/Components/ExternalComponents.hpp"
#include "Helpers/delegates.hpp"
#include "logging.hpp"
#include "BSML.hpp"

#include "UnityEngine/Object.hpp"
#include "UnityEngine/Resources.hpp"
#include "UnityEngine/RectTransform.hpp"
#include "UnityEngine/GameObject.hpp"
#include "UnityEngine/Vector2.hpp"

#include "HMUI/ImageView.hpp"
#include "BSML/Components/ModalColorPicker.hpp"

using namespace HMUI;
using namespace UnityEngine;
using namespace UnityEngine::UI;

namespace BSML {
    static BSMLNodeParser<ModalColorPickerTag> modalColorPickerTagParser({"modal-color-picker"});
    std::string buttonXML {
        "\
        <horizontal anchor-pos-y='-30' spacing='2' horizontal-fit='PreferredSize'>\
            <button text='Cancel' on-click='CancelPressed' pref-width='30'/>\
            <action-button text='Done' on-click='DonePressed' pref-width='30'/>\
        </horizontal>\
        "
    };

    GlobalNamespace::RGBPanelController* get_rgbTemplate() {
        static SafePtrUnity<GlobalNamespace::RGBPanelController> rgbTemplate;
        if (!rgbTemplate)
            rgbTemplate = Resources::FindObjectsOfTypeAll<GlobalNamespace::RGBPanelController*>()->FirstOrDefault([](auto x){ return x->get_name() == "RGBColorPicker"; });
        return rgbTemplate.ptr();
    }
    GlobalNamespace::HSVPanelController* get_hsvTemplate() {
        static SafePtrUnity<GlobalNamespace::HSVPanelController> hsvTemplate;
        if (!hsvTemplate)
            hsvTemplate = Resources::FindObjectsOfTypeAll<GlobalNamespace::HSVPanelController*>()->FirstOrDefault([](auto x){ return x->get_name() == "HSVColorPicker"; });
        return hsvTemplate.ptr();
    }
    HMUI::ImageView* get_currentColorTemplate() {
        static SafePtrUnity<HMUI::ImageView> currentColorTemplate;
        if (!currentColorTemplate) {
            currentColorTemplate = Resources::FindObjectsOfTypeAll<HMUI::ImageView*>()->FirstOrDefault([](auto x){
                if (x->get_name() != "SaberColorA") return false;
                auto parent = x->get_transform()->get_parent();
                if (!parent) return false;
                return parent->get_name() == "ColorSchemeView";
            });
        }
        return currentColorTemplate.ptr();
    }

    UnityEngine::GameObject* ModalColorPickerTag::CreateObject(UnityEngine::Transform* parent) const {
        DEBUG("Creating Modal Color Picker");

        GameObject* gameObject = Base::CreateObject(parent);
        auto externalComponents = gameObject->GetComponent<ExternalComponents*>();
        auto windowTransform = gameObject->transform.cast<RectTransform>();
        gameObject->set_name("BSMLModalColorPicker");
        windowTransform->set_sizeDelta({135, 75});

        auto colorPicker = gameObject->AddComponent<ModalColorPicker*>();
        colorPicker->modalView = externalComponents->Get<ModalView*>();



        auto onChangeInfo = il2cpp_functions::class_get_method_from_name(colorPicker->klass, "OnChange", 2);
        auto delegate = MakeSystemAction<UnityEngine::Color, GlobalNamespace::ColorChangeUIEventType>(colorPicker, onChangeInfo);
        auto rgbController = Object::Instantiate(get_rgbTemplate(), gameObject->get_transform(), false);
        rgbController->set_name("BSMLRGBPanel");
        auto rgbTransform = rgbController->transform.cast<RectTransform>();
        rgbTransform->set_anchoredPosition({0, 3});
        rgbTransform->set_anchorMin({0, .25f});
        rgbTransform->set_anchorMax({0, .25f});
        colorPicker->rgbPanel = rgbController;
        rgbController->add_colorDidChangeEvent(delegate);

        auto hsvController = Object::Instantiate(get_hsvTemplate(), gameObject->get_transform(), false);
        hsvController->set_name("BSMLHSVPanel");
        auto hsvTransform = hsvController->transform.cast<RectTransform>();
        hsvTransform->set_anchoredPosition({0, 3});
        hsvTransform->set_anchorMin({0.75f, 0.5f});
        hsvTransform->set_anchorMax({0.75f, 0.5f});
        hsvController->add_colorDidChangeEvent(delegate);
        colorPicker->hsvPanel = hsvController;

        auto colorImage = Object::Instantiate(get_currentColorTemplate(), gameObject->get_transform(), false);
        colorImage->set_name("BSMLCurrentColor");
        auto colorTransform = colorImage->transform.cast<RectTransform>();
        colorTransform->set_anchoredPosition({0, 0});
        colorTransform->set_anchorMin({0.5, 0.5f});
        colorTransform->set_anchorMax({0.5, 0.5f});
        colorPicker->colorImage = colorImage;

        BSML::parse_and_construct(buttonXML, gameObject->get_transform(), colorPicker);

        externalComponents->Add(colorPicker);
        return gameObject;
    }
}
