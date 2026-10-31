#pragma once

#include "HMUI/ImageView.hpp"
#include "bsml/shared/BSML/Components/CustomListTableData.hpp"
#include "custom-types/shared/macros.hpp"
#include "song-details/shared/SongDetails.hpp"

#include "Airbuds/AirbudsClient.hpp"

#if HOT_RELOAD
#include "bsml/shared/BSML/ViewControllers/HotReloadViewController.hpp"
using BaseViewController = BSML::HotReloadViewController;
#else
#include "HMUI/ViewController.hpp"
using BaseViewController = HMUI::ViewController;
#endif

DECLARE_CLASS_CODEGEN_INTERFACES(AirbudsSearch::UI::ViewControllers, DownloadHistoryViewController, BaseViewController) {

    DECLARE_OVERRIDE_METHOD_MATCH(void, DidActivate, &HMUI::ViewController::DidActivate, bool isFirstActivation, bool addedToHierarchy, bool screenSystemDisabling);

    DECLARE_INSTANCE_METHOD(void, PostParse);

    // BSML
    DECLARE_INSTANCE_FIELD(UnityW<BSML::CustomListTableData>, customSongsList_);
};
