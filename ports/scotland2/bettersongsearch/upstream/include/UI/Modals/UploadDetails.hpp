#pragma once

#include "bsml/shared/macros.hpp"
#include "bsml/shared/BSML.hpp"
#include "TMPro/TextMeshProUGUI.hpp"
#include "HMUI/ModalView.hpp"
#include "HMUI/ImageView.hpp"
#include "HMUI/TextPageScrollView.hpp"
#include "bsml/shared/BSML/Components/Settings/SliderSetting.hpp"
#include "bsml/shared/BSML/Components/ModalView.hpp"
#include "bsml/shared/BSML/ViewControllers/HotReloadViewController.hpp"
#include "custom-types/shared/coroutine.hpp"
#include "custom-types/shared/macros.hpp"
#include "UnityEngine/MonoBehaviour.hpp"
#include "Util/RatelimitCoroutine.hpp"
#include "song-details/shared/Data/Song.hpp"
#include "song-details/shared/Data/SongDifficulty.hpp"
#include "song-details/shared/Data/MapCharacteristic.hpp"

DECLARE_CLASS_CODEGEN(BetterSongSearch::UI::Modals, UploadDetails, UnityEngine::MonoBehaviour) {
    DECLARE_INSTANCE_METHOD(void, OnEnable);
    DECLARE_CTOR(ctor);

    DECLARE_INSTANCE_METHOD(void, CloseModal);
    DECLARE_INSTANCE_METHOD(void, OpenBeatSaver);
    DECLARE_INSTANCE_METHOD(void, CopyBSR);
    DECLARE_INSTANCE_METHOD(void, OpenMapPreview);


    DECLARE_INSTANCE_FIELD(TMPro::TextMeshProUGUI*, selectedSongKey);
    DECLARE_INSTANCE_FIELD(TMPro::TextMeshProUGUI*, selectedRating);
    DECLARE_INSTANCE_FIELD(TMPro::TextMeshProUGUI*, selectedCharacteristics);
    DECLARE_INSTANCE_FIELD(HMUI::TextPageScrollView*, selectedSongDescription);
    DECLARE_INSTANCE_FIELD(HMUI::ImageView*, songDetailsLoading);

    DECLARE_INSTANCE_FIELD(bool, initialized);

    DECLARE_INSTANCE_FIELD(BSML::ModalView*, rootModal);
    public:
        void OpenModal(const SongDetailsCache::Song* song);
};
