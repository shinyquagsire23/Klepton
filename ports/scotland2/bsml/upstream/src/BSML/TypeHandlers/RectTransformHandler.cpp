#include "BSML/TypeHandlers/RectTransformHandler.hpp"
#include "Helpers/utilities.hpp"
#include "UnityEngine/Vector2.hpp"
#include "UnityEngine/GameObject.hpp"
#include "HMUI/HoverHint.hpp"
#include "Helpers/getters.hpp"

void AddHoverHint(UnityEngine::RectTransform* rectTransform, const std::string& text) {
    auto hoverHint = rectTransform->GetComponent<HMUI::HoverHint*>();
    if (!hoverHint) {
        hoverHint = rectTransform->get_gameObject()->AddComponent<HMUI::HoverHint*>();
    }
    hoverHint->set_text(text);
    hoverHint->_hoverHintController = BSML::Helpers::GetHoverHintController();
}

// for now, because I wanna have the option later
void AddHoverHintKey(UnityEngine::RectTransform* rectTransform, const std::string& key) {
    AddHoverHint(rectTransform, key);
}

namespace BSML {
    static RectTransformHandler rectTransformHandler{};

    RectTransformHandler::Base::PropMap RectTransformHandler::get_props() const {
        return {
            {"anchorMinX", {"anchor-min-x"}},
            {"anchorMinY", {"anchor-min-y"}},
            {"anchorMin",  {"anchor-min"}},
            {"anchorMaxX", {"anchor-max-x"}},
            {"anchorMaxY", {"anchor-max-y"}},
            {"anchorMax",  {"anchor-max"}},
            {"anchorPosX", {"anchor-pos-x"}},
            {"anchorPosY", {"anchor-pos-y"}},
            {"anchorPos",  {"anchor-pos"}},
            {"sizeDeltaX", {"size-delta-x"}},
            {"sizeDeltaY", {"size-delta-y"}},
            {"sizeDelta", {"size-delta"}},
            {"pivotX", {"pivot-x"}},
            {"pivotY", {"pivot-y"}},
            {"pivot", {"pivot"}},
            {"hoverHint", {"hover-hint"}},
            {"hoverHintKey", {"hover-hint-key"}},
            {"active", {"active"}},
            {"localScale", {"local-scale", "scale" }},
        };
    }

    RectTransformHandler::Base::SetterMap RectTransformHandler::get_setters() const {
        return {
            {"anchorMinX",      [](auto component, auto value){ component->set_anchorMin(UnityEngine::Vector2(value, component->get_anchorMin().y)); }},
            {"anchorMinY",      [](auto component, auto value){ component->set_anchorMin(UnityEngine::Vector2(component->get_anchorMin().x, value)); }},
            {"anchorMin",       [](auto component, auto value){ component->set_anchorMin(value.tryParseVector2().value_or(UnityEngine::Vector2(0, 0))); }},
            {"anchorMaxX",      [](auto component, auto value){ component->set_anchorMax(UnityEngine::Vector2(value, component->get_anchorMax().y)); }},
            {"anchorMaxY",      [](auto component, auto value){ component->set_anchorMax(UnityEngine::Vector2(component->get_anchorMax().x, value)); }},
            {"anchorMax",       [](auto component, auto value){ component->set_anchorMax(value.tryParseVector2().value_or(UnityEngine::Vector2(1, 1))); }},
            {"anchorPosX",      [](auto component, auto value){ component->set_anchoredPosition(UnityEngine::Vector2(value, component->get_anchoredPosition().y)); }},
            {"anchorPosY",      [](auto component, auto value){ component->set_anchoredPosition(UnityEngine::Vector2(component->get_anchoredPosition().x, value)); }},
            {"sizeDeltaX",      [](auto component, auto value){ component->set_sizeDelta(UnityEngine::Vector2(value, component->get_sizeDelta().y)); }},
            {"sizeDeltaY",      [](auto component, auto value){ component->set_sizeDelta(UnityEngine::Vector2(component->get_sizeDelta().x, value)); }},
            {"sizeDelta",       [](auto component, auto value){ component->set_sizeDelta(value.tryParseVector2().value_or(UnityEngine::Vector2(0, 0))); }},
            {"pivotX",          [](auto component, auto value){ component->set_pivot(UnityEngine::Vector2(value, component->get_pivot().y)); }},
            {"pivotY",          [](auto component, auto value){ component->set_pivot(UnityEngine::Vector2(component->get_pivot().x, value)); }},
            {"pivot",           [](auto component, auto value){ component->set_sizeDelta(value.tryParseVector2().value_or(UnityEngine::Vector2(0.5, 0.5))); }},
            {"hoverHint",       [](auto component, auto value){ AddHoverHint(component, value); }},
            {"hoverHintKey",    [](auto component, auto value){ AddHoverHintKey(component, value); }},
            {"active",          [](auto component, auto value){ component->get_gameObject()->SetActive(value); }},
            {"localScale",      [](auto component, auto value){ component->localScale = value.tryParseVector3(1).value_or(UnityEngine::Vector3(1, 1, 1)); }}
        };
    }
}
