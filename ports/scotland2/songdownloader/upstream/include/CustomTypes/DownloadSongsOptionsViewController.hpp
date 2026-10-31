#pragma once

#include "custom-types/shared/macros.hpp"

#include "HMUI/ViewController.hpp"
#include "bsml/shared/BSML/Components/Settings/DropdownListSetting.hpp"

DECLARE_CLASS_CODEGEN(SongDownloader, DownloadSongsOptionsViewController, HMUI::ViewController) {
    DECLARE_INSTANCE_FIELD(BSML::DropdownListSetting*, Automapper);
    DECLARE_INSTANCE_FIELD(BSML::DropdownListSetting*, Service);
    DECLARE_INSTANCE_FIELD(BSML::DropdownListSetting*, ListType_BeatSaver);
    DECLARE_INSTANCE_FIELD(BSML::DropdownListSetting*, ListType_ScoreSaber);
    DECLARE_INSTANCE_FIELD(BSML::DropdownListSetting*, SortOrder);
    DECLARE_INSTANCE_FIELD(BSML::DropdownListSetting*, NEdropdown);
    DECLARE_INSTANCE_FIELD(BSML::DropdownListSetting*, MEdropdown);
    DECLARE_INSTANCE_FIELD(BSML::DropdownListSetting*, Chroma);
    DECLARE_INSTANCE_FIELD(BSML::DropdownListSetting*, Ranked);

    std::string LastListType;

    DECLARE_OVERRIDE_METHOD_MATCH(void, DidActivate, &HMUI::ViewController::DidActivate, bool firstActivation, bool addedToHierarchy, bool screenSystemEnabling);
};
