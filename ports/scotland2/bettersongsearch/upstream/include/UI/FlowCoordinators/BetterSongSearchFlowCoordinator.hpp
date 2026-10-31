#pragma once

#include "HMUI/ViewController.hpp"
#include "HMUI/FlowCoordinator.hpp"
#include "custom-types/shared/macros.hpp"
#include "beatsaber-hook/shared/utils/il2cpp-utils.hpp"

#include "UI/ViewControllers/SongList.hpp"
#include "UI/ViewControllers/FilterView.hpp"
#include "UI/ViewControllers/DownloadHistory.hpp"


DECLARE_CLASS_CODEGEN(BetterSongSearch::UI::FlowCoordinators, BetterSongSearchFlowCoordinator, HMUI::FlowCoordinator) {
    DECLARE_INSTANCE_FIELD(UnityW<ViewControllers::SongListController>, SongListController);
    DECLARE_INSTANCE_FIELD(UnityW<ViewControllers::FilterViewController>, FilterViewController);
    DECLARE_INSTANCE_FIELD(UnityW<ViewControllers::DownloadHistoryViewController>, DownloadHistoryViewController);

    DECLARE_INSTANCE_METHOD(void, Awake);

    DECLARE_OVERRIDE_METHOD_MATCH(void, DidActivate, &HMUI::FlowCoordinator::DidActivate, bool firstActivation, bool addedToHeirarchy, bool screenSystemEnabling);
    DECLARE_OVERRIDE_METHOD_MATCH(void, BackButtonWasPressed, &HMUI::FlowCoordinator::BackButtonWasPressed, HMUI::ViewController* topViewController);

    public:

    std::function<void()> cancelConfirmCallback;
    void Close(bool immediately = false, bool downloadAbortConfim = true);
    bool ConfirmCancelOfPending(std::function<void()> callback);
    void ConfirmCancelCallback(bool doCancel = true);
};

inline UnityW<BetterSongSearch::UI::FlowCoordinators::BetterSongSearchFlowCoordinator> fcInstance;
