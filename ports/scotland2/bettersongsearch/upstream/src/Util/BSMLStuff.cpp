#include "Util/BSMLStuff.hpp"

#include "custom-types/shared/coroutine.hpp"
#include "HMUI/CurvedTextMeshPro.hpp"
#include "HMUI/CustomFormatRangeValuesSlider.hpp"
#include "HMUI/ScrollView.hpp"
#include "UnityEngine/GameObject.hpp"
#include "UnityEngine/RectTransform.hpp"
#include "UnityEngine/UI/LayoutElement.hpp"
#include "UnityEngine/WaitForEndOfFrame.hpp"

#define coro(coroutine) BSML::SharedCoroutineStarter::get_instance()->StartCoroutine(custom_types::Helpers::CoroutineHelper::New(coroutine))

using namespace UnityEngine;

namespace BetterSongSearch::UI::Util::BSMLStuff {
    custom_types::Helpers::Coroutine MergeSliders(GameObject* container, bool constrictValuesMinMax) {
        co_yield nullptr;
        auto items = container->GetComponentsInChildren<HMUI::CurvedTextMeshPro*>();

        // Construct a list of MERGE_TO_PREV
        std::vector<HMUI::CurvedTextMeshPro*> filteredItems = std::vector<HMUI::CurvedTextMeshPro*>();

        for (auto item : items) {
            if (item != nullptr && item->___m_CachedPtr.m_value != nullptr && item->get_text() == "MERGE_TO_PREV") {
                filteredItems.push_back(item);
            }
        }

        for (auto x : filteredItems) {
            co_yield reinterpret_cast<System::Collections::IEnumerator*>(WaitForEndOfFrame::New_ctor());
            auto ourContainer = x->get_transform()->get_parent();
            auto prevContainer = ourContainer->get_parent()->GetChild(ourContainer->GetSiblingIndex() - 1);

            prevContainer->Find("BSMLSlider").cast<UnityEngine::RectTransform>()->set_offsetMax(Vector2(-20, 0));
            ourContainer->Find("BSMLSlider").cast<UnityEngine::RectTransform>()->set_offsetMin(Vector2(-20, 0));
            ourContainer->set_position(prevContainer->get_position());

            auto minTimeSlider = prevContainer->GetComponentInChildren<HMUI::CustomFormatRangeValuesSlider*>();
            auto maxTimeSlider = ourContainer->GetComponentInChildren<HMUI::CustomFormatRangeValuesSlider*>();

            if (minTimeSlider == nullptr || maxTimeSlider == nullptr) {
                co_yield nullptr;
            } else {
                auto newSize = minTimeSlider->get_valueSize() / 2.1f;
                maxTimeSlider->set_valueSize(newSize);
                minTimeSlider->set_valueSize(newSize);
                ourContainer->GetComponentInChildren<UnityEngine::UI::LayoutElement*>()->set_ignoreLayout(true);
                x->set_text("");
            }
        }
    }

    void SetStringSettingValue(BSML::StringSetting* element, std::string value) {
        // auto formatter = element->formatter;
        // auto formattedValue = formatter(formatter ? formatter(value) : StringW(value));
        // element->modalKeyboard->SetText(value);
        element->set_text(value);
        element->ApplyValue();
    }

    void SetSliderSettingValue(BSML::SliderSetting* element, float value) {
        // auto formatter = element->formatter;
        // auto formattedValue = formatter(formatter ? formatter(value) : value);
        // element->slider->set_value(value);
        element->set_Value(value);
        element->ApplyValue();
    }

    void FormatSliderSettingValue(BSML::SliderSetting* element) {
        // Not pretty but works
        auto value = element->get_Value();
        auto formatter = element->formatter;
        element->text->set_text(formatter ? formatter(value) : StringW(fmt::format("{}", value)));
    }

    void FormatStringSettingValue(BSML::StringSetting* element) {
        // Not pretty but works
        auto value = element->get_text();
        auto formatter = element->formatter;
        element->text->set_text(formatter ? formatter(value) : value);
    }
};  // namespace BetterSongSearch::UI::Util::BSMLStuff
