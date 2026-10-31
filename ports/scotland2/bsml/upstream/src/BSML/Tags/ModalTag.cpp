#include "BSML/Tags/ModalTag.hpp"
#include "BSML/Components/ModalView.hpp"
#include "BSML/Components/ExternalComponents.hpp"

#include "Helpers/getters.hpp"
#include "logging.hpp"

#include "UnityEngine/GameObject.hpp"
#include "UnityEngine/Resources.hpp"
#include "UnityEngine/RectTransform.hpp"
#include "UnityEngine/UI/Image.hpp"
#include "UnityEngine/UI/ScrollRect.hpp"
#include "UnityEngine/Vector2.hpp"
#include "HMUI/TableView.hpp"
#include "HMUI/ScrollView.hpp"
#include "HMUI/EventSystemListener.hpp"
#include "VRUIControls/VRGraphicRaycaster.hpp"

using namespace HMUI;
using namespace UnityEngine;
using namespace UnityEngine::UI;
using namespace VRUIControls;

namespace BSML {
    static BSMLNodeParser<ModalTag> modalTagParser({"modal"});

    HMUI::ModalView* get_modalViewTemplate() {
        static SafePtrUnity<HMUI::ModalView> modalViewTemplate;
        if (!modalViewTemplate) {
            modalViewTemplate = Resources::FindObjectsOfTypeAll<HMUI::ModalView*>()->FirstOrDefault([](auto x){ return x->get_gameObject()->get_name() == "DropdownTableView"; });
        }
        return modalViewTemplate.ptr();
    }

    UnityEngine::GameObject* ModalTag::CreateObject(UnityEngine::Transform* parent) const {
        DEBUG("Creating Modal");
        auto modalViewTemplate = get_modalViewTemplate();
        auto copy = Helpers::GetDiContainer()->InstantiatePrefab(modalViewTemplate, parent);
        auto gameObject = copy->get_gameObject();
        gameObject->SetActive(false);
        gameObject->set_name("BSMLModalView");
        // we use our own custom modalView type, this differs from PC BSML but it just makes it easier to set things up
        auto modalView = Helpers::GetDiContainer()->InstantiateComponent<BSML::ModalView*>(gameObject);

        modalView->_presentPanelAnimations = modalViewTemplate->_presentPanelAnimations;
        modalView->_dismissPanelAnimation = modalViewTemplate->_dismissPanelAnimation;
        gameObject->GetComponent<VRGraphicRaycaster*>()->_physicsRaycaster = Helpers::GetPhysicsRaycasterWithCache();

        Object::DestroyImmediate(gameObject->GetComponent<TableView*>());
        Object::DestroyImmediate(gameObject->GetComponent<ScrollRect*>());
        Object::DestroyImmediate(gameObject->GetComponent<ScrollView*>());
        Object::DestroyImmediate(gameObject->GetComponent<EventSystemListener*>());

        auto rectTransform = modalView->transform.cast<RectTransform>();
        int childCount = rectTransform->get_childCount();

        for (int i = 0; i < childCount; i++) {
            auto child = rectTransform->GetChild(i).cast<RectTransform>();
            DEBUG("child name: {}", child->get_name());

            if (child->get_name() == "BG") {
                DEBUG("twas BG");
                child->set_anchoredPosition({0, 0});
                child->set_sizeDelta({0, 0});
                child->GetComponent<Image*>()->set_raycastTarget(true);
                child->get_gameObject()->SetActive(true);
            } else {
                DEBUG("Destroyed!");
                Object::Destroy(child->get_gameObject());
            }
        }

        rectTransform->set_anchorMin({.5f, .5f});
        rectTransform->set_anchorMax({.5f, .5f});
        rectTransform->set_sizeDelta({0, 0});

        auto externalComponents = gameObject->AddComponent<BSML::ExternalComponents*>();
        externalComponents->Add(rectTransform);
        externalComponents->Add(modalView);

        return gameObject;
    }
}
