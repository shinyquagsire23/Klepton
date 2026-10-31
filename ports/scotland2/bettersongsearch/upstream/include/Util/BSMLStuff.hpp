#pragma once

#include "bsml/shared/BSML.hpp"
#include "bsml/shared/BSML/Components/Settings/SliderSetting.hpp"
#include "bsml/shared/BSML/Components/Settings/StringSetting.hpp"
#include "bsml/shared/macros.hpp"
#include "custom-types/shared/coroutine.hpp"
#include "custom-types/shared/macros.hpp"
#include "HMUI/VerticalScrollIndicator.hpp"
#include "System/Action.hpp"
#include "System/Collections/IEnumerator.hpp"
#include "UnityEngine/Coroutine.hpp"
#include "UnityEngine/GameObject.hpp"
#include "UnityEngine/MonoBehaviour.hpp"

namespace BetterSongSearch::UI::Util::BSMLStuff {
    custom_types::Helpers::Coroutine MergeSliders(UnityEngine::GameObject* container, bool constrictValuesMinMax = true);

    void SetStringSettingValue(BSML::StringSetting* element, std::string value);
    void SetSliderSettingValue(BSML::SliderSetting* element, float value);
    void FormatSliderSettingValue(BSML::SliderSetting* element);
    void FormatStringSettingValue(BSML::StringSetting* element);
}  // namespace BetterSongSearch::UI::Util::BSMLStuff
