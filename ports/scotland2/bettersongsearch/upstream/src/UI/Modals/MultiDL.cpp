#include "UI/Modals/MultiDL.hpp"

#include "assets.hpp"
#include "bsml/shared/BSML.hpp"
#include "DataHolder.hpp"
#include "logging.hpp"
#include "songcore/shared/SongCore.hpp"
#include "System/Tuple_2.hpp"
#include "UI/FlowCoordinators/BetterSongSearchFlowCoordinator.hpp"
#include "Util/CurrentTimeMs.hpp"

using namespace BetterSongSearch::UI;
using namespace BetterSongSearch::Util;
#define coro(coroutine) BSML::SharedCoroutineStarter::get_instance()->StartCoroutine(custom_types::Helpers::CoroutineHelper::New(coroutine))

DEFINE_TYPE(BetterSongSearch::UI::Modals, MultiDL);

void Modals::MultiDL::OnEnable() {
}

void Modals::MultiDL::StartMultiDownload() {
    DEBUG("Downloading {} songs", this->songsToDownload);

    auto songController = fcInstance->SongListController;
    auto dlController = fcInstance->DownloadHistoryViewController;
    auto table = songController->songListTable();
    auto range = table->GetVisibleCellsIdRange();

    int downloaded = 0;
    auto songList = dataHolder.GetDisplayedSongList();  // Get the displayed song list copy
    for (int i = range->get_Item1(); i < songList.size(); i++) {
        auto song = songList[i];
        // Skip if can't dl or already downloading
        if (dlController->CheckIsDownloaded(std::string(song->hash())) || !dlController->CheckIsDownloadable(std::string(song->hash()))) {
            continue;
        }

        if (!dlController->TryAddDownload(song, true)) {
            continue;
        }

        if (++downloaded >= this->songsToDownload) {
            break;
        }
    }
    dlController->RefreshTable(true);
    this->CloseModal();
    return;
}

void Modals::MultiDL::CloseModal() {
    this->modal->Hide();
}

void Modals::MultiDL::ctor() {
    INVOKE_CTOR();
    this->initialized = false;
    this->songsToDownload = 5;
}

void Modals::MultiDL::OpenModal() {
    if (!initialized) {
        BSML::parse_and_construct(Assets::MultiDl_bsml, this->get_transform(), this);
        initialized = true;
    }
    this->modal->Show();
}
