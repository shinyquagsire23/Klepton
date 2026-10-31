#pragma once

#include "bsml/shared/macros.hpp"
#include "bsml/shared/BSML.hpp"
#include "HMUI/ModalView.hpp"
#include "bsml/shared/BSML/Components/Settings/SliderSetting.hpp"
#include "bsml/shared/BSML/Components/ModalView.hpp"
#include "bsml/shared/BSML/ViewControllers/HotReloadViewController.hpp"
#include "custom-types/shared/coroutine.hpp"
#include "custom-types/shared/macros.hpp"
#include "UnityEngine/MonoBehaviour.hpp"
#include "Util/RatelimitCoroutine.hpp"

DECLARE_CLASS_CODEGEN(BetterSongSearch::UI::Modals, Settings, UnityEngine::MonoBehaviour) {
    DECLARE_INSTANCE_METHOD(void, OnEnable);
    DECLARE_CTOR(ctor);

    DECLARE_INSTANCE_METHOD(void, CloseModal);
    DECLARE_INSTANCE_METHOD(void, OpenModal);

    DECLARE_INSTANCE_FIELD(bool, initialized);

    DECLARE_BSML_PROPERTY(bool, returnToBssFromSolo);
    DECLARE_BSML_PROPERTY(bool, loadSongPreviews);
    DECLARE_BSML_PROPERTY(bool, smallerFontSize);

    DECLARE_BSML_PROPERTY(StringW, preferredLeaderboard);
    DECLARE_INSTANCE_FIELD(BSML::ModalView*, settingsModal);
    BSML_OPTIONS_LIST_OBJECT(preferredLeaderboardOptions, "Scoresaber", "Beatleader");
};
