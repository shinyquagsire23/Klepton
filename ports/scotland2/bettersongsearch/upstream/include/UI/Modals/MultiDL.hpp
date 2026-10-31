#pragma once

#include "HMUI/ViewController.hpp"
#include "bsml/shared/macros.hpp"
#include "bsml/shared/BSML.hpp"
#include "HMUI/ModalView.hpp"
#include "bsml/shared/BSML/Components/Settings/SliderSetting.hpp"
#include "bsml/shared/BSML/Components/ModalView.hpp"
#include "bsml/shared/BSML/ViewControllers/HotReloadViewController.hpp"
#include "custom-types/shared/coroutine.hpp"
#include "custom-types/shared/macros.hpp"
#include "UnityEngine/MonoBehaviour.hpp"

#define GET_FIND_METHOD(mPtr) il2cpp_utils::il2cpp_type_check::MetadataGetter<mPtr>::get()

DECLARE_CLASS_CODEGEN(BetterSongSearch::UI::Modals, MultiDL, UnityEngine::MonoBehaviour) {
    DECLARE_INSTANCE_METHOD(void, OnEnable);
    DECLARE_CTOR(ctor);

    DECLARE_INSTANCE_METHOD(void, StartMultiDownload);
    DECLARE_INSTANCE_METHOD(void, CloseModal);
    DECLARE_INSTANCE_METHOD(void, OpenModal);

    DECLARE_INSTANCE_FIELD(bool, initialized);
    DECLARE_INSTANCE_FIELD(StringW, existingSongs);
    DECLARE_INSTANCE_FIELD(int, songsToDownload);
    DECLARE_INSTANCE_FIELD(BSML::ModalView*, modal);

    // Sliders that need to be formatted
    DECLARE_INSTANCE_FIELD(BSML::SliderSetting*, multiDlCountSlider);
};
