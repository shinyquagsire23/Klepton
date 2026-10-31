#include "UI/ViewControllers/SongList.hpp"

#include <algorithm>
#include <cstdint>
#include <future>
#include <shared_mutex>
#include <string>

#include "assets.hpp"
#include "BeatSaverRegionManager.hpp"
#include "bsml/shared/BSML.hpp"
#include "bsml/shared/BSML/MainThreadScheduler.hpp"
#include "bsml/shared/BSML/SharedCoroutineStarter.hpp"
#include "bsml/shared/Helpers/delegates.hpp"
#include "bsml/shared/Helpers/getters.hpp"
#include "bsml/shared/Helpers/utilities.hpp"
#include "DataHolder.hpp"
#include <fmt/core.h>
#include "GlobalNamespace/LevelCollectionNavigationController.hpp"
#include "GlobalNamespace/LevelCollectionTableView.hpp"
#include "GlobalNamespace/LevelCollectionViewController.hpp"
#include "GlobalNamespace/LevelSearchViewController.hpp"
#include "GlobalNamespace/LevelSelectionFlowCoordinator.hpp"
#include "GlobalNamespace/LevelSelectionNavigationController.hpp"
#include "GlobalNamespace/PlayerData.hpp"
#include "GlobalNamespace/PlayerDataModel.hpp"
#include "GlobalNamespace/SelectLevelCategoryViewController.hpp"
#include "GlobalNamespace/SongPreviewPlayer.hpp"
#include "HMUI/CurvedTextMeshPro.hpp"
#include "HMUI/InputFieldViewChangeBinder.hpp"
#include "HMUI/TableView.hpp"
#include "logging.hpp"
#include "PluginConfig.hpp"
#include "songcore/shared/SongCore.hpp"
#include "songcore/shared/SongLoader/RuntimeSongLoader.hpp"
#include "System/Action.hpp"
#include "System/Nullable_1.hpp"
#include "UI/FlowCoordinators/BetterSongSearchFlowCoordinator.hpp"
#include "UI/Manager.hpp"
#include "UnityEngine/AudioClip.hpp"
#include "UnityEngine/AudioType.hpp"
#include "UnityEngine/Networking/DownloadHandlerAudioClip.hpp"
#include "UnityEngine/Networking/UnityWebRequest.hpp"
#include "UnityEngine/Networking/UnityWebRequestMultimedia.hpp"
#include "UnityEngine/Resources.hpp"
#include "UnityEngine/WaitForSeconds.hpp"
#include "Util/BSMLStuff.hpp"
#include "Util/CurrentTimeMs.hpp"
#include "Util/Debug.hpp"
#include "Util/Random.hpp"
#include "Util/SongUtil.hpp"
#include "Util/TextUtil.hpp"

#define coro(coroutine) BSML::SharedCoroutineStarter::get_instance()->StartCoroutine(custom_types::Helpers::CoroutineHelper::New(coroutine))

using namespace BetterSongSearch::UI;
using namespace BetterSongSearch::Util;
using namespace BetterSongSearch::UI::Util::BSMLStuff;
using namespace GlobalNamespace;
using namespace SongDetailsCache;
using namespace UnityEngine;

#define SONGDOWNLOADER

SongPreviewPlayer* songPreviewPlayer = nullptr;
LevelCollectionViewController* levelCollectionViewController = nullptr;

DEFINE_TYPE(ViewControllers, SongListController);

std::vector<std::string> const CHAR_GROUPING = {
    "Unknown", "Standard", "OneSaber", "NoArrows", "Lightshow", "NintyDegree", "ThreeSixtyDegree", "Lawless"
};
std::vector<std::string> const CHAR_FILTER_OPTIONS = {
    "Any", "Custom", "Standard", "One Saber", "No Arrows", "90 Degrees", "360 Degrees", "Lightshow", "Lawless"
};
std::vector<std::string> const DIFFS = {"Easy", "Normal", "Hard", "Expert", "Expert+"};
std::vector<std::string> const REQUIREMENTS = {"Any", "Noodle Extensions", "Mapping Extensions", "Chroma", "Cinema"};

void ViewControllers::SongListController::_UpdateSearchedSongsList() {
    dataHolder.Search();
}

void ViewControllers::SongListController::UpdateSearchedSongsList() {
    this->StartCoroutine(custom_types::Helpers::CoroutineHelper::New(limitedUpdateSearchedSongsList->CallNextFrame()));
}

void ViewControllers::SongListController::PostParse() {
    // Steal search box from the base game
    UnityW<HMUI::InputFieldView> gameSearchBox;
    gameSearchBox = BSML::Helpers::GetDiContainer()->Resolve<LevelSearchViewController*>()->_searchTextInputFieldView;

    if (gameSearchBox) {
        DEBUG("Found search box");
        // Cleanup the old search
        if (searchBox) {
            UnityEngine::Object::DestroyImmediate(searchBox);
        }
        searchBox = Instantiate(gameSearchBox->get_gameObject(), searchBoxContainer->get_transform(), false);
        UnityW<HMUI::InputFieldView> songSearchInput = searchBox->GetComponent<HMUI::InputFieldView*>();
        songSearchPlaceholder = searchBox->get_transform()->Find("PlaceholderText")->GetComponent<HMUI::CurvedTextMeshPro*>();
        songSearchPlaceholder->set_text("Search by Song, Key, Mapper..");
        songSearchInput->____keyboardPositionOffset = Vector3(-15, -36, 0);
        songSearchInput->set_text("");  // Clear the text

        std::function<void(UnityW<HMUI::InputFieldView> view)> onValueChanged = [this](UnityW<HMUI::InputFieldView> view) {
            DEBUG("Input is: {}", (std::string) view->get_text());
            this->SortAndFilterSongs(dataHolder.sort, (std::string) view->get_text(), true);
        };

        songSearchInput->get_onValueChanged()->AddListener(BSML::MakeUnityAction(onValueChanged));
    } else {
        ERROR("Search box not found");
    }

    // Get the default cover image
    defaultImage = BSML::Utilities::LoadSpriteRaw(Assets::CustomLevelsCover_png);
    // Set default cover image
    coverImage->set_sprite(defaultImage);

    // Get song preview player
    songPreviewPlayer = BSML::Helpers::GetDiContainer()->Resolve<SongPreviewPlayer*>();
    levelCollectionViewController = BSML::Helpers::GetDiContainer()->Resolve<LevelCollectionViewController*>();

    // BSML has a bug that stops getting the correct platform helper and on game reset it dies and the scrollhelper stays invalid and scroll doesn't
    // work
    auto platformHelper = BSML::Helpers::GetDiContainer()->Resolve<GlobalNamespace::IVRPlatformHelper*>();
    if (platformHelper == nullptr) {
    } else {
        for (auto x : this->GetComponentsInChildren<HMUI::ScrollView*>()) {
            x->____platformHelper = platformHelper;
        }
    }

    // Make the sort dropdown bigger
    auto c = std::min(9, this->get_sortModeSelections()->____size);
    sortDropdown->dropdown->____numberOfVisibleCells = c;
    sortDropdown->dropdown->ReloadData();
    auto m = sortDropdown->dropdown->____modalView;
    m->get_transform().cast<RectTransform>()->set_pivot(UnityEngine::Vector2(0.5f, 0.83f + (c * 0.011f)));

    if (this->songList) {
        songList->tableView->SetDataSource(reinterpret_cast<HMUI::TableView::IDataSource*>(this), false);
    }
}

void ViewControllers::SongListController::DidActivate(bool firstActivation, bool addedToHeirarchy, bool screenSystemDisabling) {
    // Init everything before we call events
    if (firstActivation) {
        DEBUG("SongListController first activation");
        if (dataHolder.playerDataModel == nullptr) {
            dataHolder.playerDataModel = BSML::Helpers::GetDiContainer()->Resolve<GlobalNamespace::PlayerDataModel*>();
        };

        // Get coordinators
        soloFreePlayFlowCoordinator = BSML::Helpers::GetDiContainer()->Resolve<SoloFreePlayFlowCoordinator*>();
        multiplayerLevelSelectionFlowCoordinator = BSML::Helpers::GetDiContainer()->Resolve<MultiplayerLevelSelectionFlowCoordinator*>();

        // Get regional beat saver urls
        BeatSaverRegionManager::RegionLookup();

        limitedUpdateSearchedSongsList = new BetterSongSearch::Util::RatelimitCoroutine(
            [this]() {
                DEBUG("UpdateSearchedSongsList limited called");
                this->_UpdateSearchedSongsList();
            },
            0.1f
        );

        IsSearching = false;
        INFO("Song list contoller activated");

        // Get sort setting from config
        auto sortMode = getPluginConfig().SortMode.GetValue();
        if (sortMode < get_sortModeSelections()->get_Count()) {
            selectedSortMode = get_sortModeSelections()->get_Item(sortMode);
            dataHolder.sort = (FilterTypes::SortMode) sortMode;
        }

        BSML::parse_and_construct(Assets::SongList_bsml, this->get_transform(), this);

        multiDlModal = this->get_gameObject()->AddComponent<UI::Modals::MultiDL*>();
        settingsModal = this->get_gameObject()->AddComponent<UI::Modals::Settings*>();
        uploadDetailsModal = this->get_gameObject()->AddComponent<UI::Modals::UploadDetails*>();

        // If loaded, refresh
        if (dataHolder.loaded) {
            DEBUG("Loaded is true");
            // Initial search
            dataHolder.forceReload = true;
            fcInstance->SongListController->SortAndFilterSongs(dataHolder.sort, dataHolder.search, true);
            fcInstance->FilterViewController->datasetInfoLabel->set_text(fmt::format("{} songs in dataset ", dataHolder.songDetails->songs.size()));
        } else {
            this->DownloadSongList();
        }

#ifdef HotReload
        fileWatcher->filePath = "/sdcard/bsml/BetterSongSearch/SongList.bsml";
        fileWatcher->checkInterval = 0.5f;
#endif
    }
    // End first activation

    dataHolder.UpdatePlayerScores();

    fromBSS = false;

    // Retry if failed to dl
    this->RetryDownloadSongList();

    // If needs a refresh, start a new search when activated
    if (dataHolder.needsRefresh) {
        // Initial search
        dataHolder.needsRefresh = false;  // Clear the flag
        dataHolder.forceReload = true;
        fcInstance->SongListController->SortAndFilterSongs(dataHolder.sort, dataHolder.search, true);
        fcInstance->FilterViewController->datasetInfoLabel->set_text(fmt::format("{} songs in dataset ", dataHolder.songDetails->songs.size()));
    }

    // Restore search songs count
    if (dataHolder.loaded && !dataHolder.failed && songSearchPlaceholder) {
        if (dataHolder.GetDisplayedSongListLength() < dataHolder.songDetails->songs.size()) {
            songSearchPlaceholder->set_text(fmt::format("Search {} songs", dataHolder.GetDisplayedSongListLength()));
        } else {
            songSearchPlaceholder->set_text("Search by Song, Key, Mapper..");
        }
    }
}

void ViewControllers::SongListController::SelectSongByHash(std::string hash) {
    if (dataHolder.songDetails == nullptr) {
        DEBUG("Song details is not loaded yet");
        return;
    }

    DEBUG("Song hash: {}", hash);
    auto& song = dataHolder.songDetails->songs.FindByHash(hash);
    if (song == SongDetailsCache::Song::none) {
        DEBUG("Uh oh, you somehow downloaded a song that was only a figment of your imagination");
        return;
    }

    SetSelectedSong(&song);
}

void ViewControllers::SongListController::SelectSong(UnityW<HMUI::TableView> table, int id) {
    if (!table) {
        return;
    }
    DEBUG("Cell clicked {}", id);
    auto song = dataHolder.GetDisplayedSongByIndex(id);
    if (song == nullptr) {
        WARNING("Song is null, requested id: {}", id);
        return;
    }
    DEBUG("Selecting song {}", id);
    this->SetSelectedSong(song);
}

float ViewControllers::SongListController::CellSize() {
    // TODO: Different font sizes
    bool smallFont = getPluginConfig().SmallerFontSize.GetValue();
    return smallFont ? 11.66f : 14.0f;
}

void ViewControllers::SongListController::ResetTable() {
    if (songListTable()) {
        songListTable()->ReloadData();
        songListTable()->ScrollToCellWithIdx(0, HMUI::TableView::ScrollPositionType::Beginning, false);
    }
}

int ViewControllers::SongListController::NumberOfCells() {
    return dataHolder.GetDisplayedSongListLength();
}

void ViewControllers::SongListController::ctor() {
    DEBUG("SongListController ctor");
    INVOKE_CTOR();
    selectedSortMode = StringW("Newest");

    // Sub to events
    dataHolder.loadingFinished += {&ViewControllers::SongListController::SongDataDone, this};
    dataHolder.loadingFailed += {&ViewControllers::SongListController::SongDataError, this};
    dataHolder.searchEnded += {&ViewControllers::SongListController::SearchDone, this};
    dataHolder.playerDataLoaded += {&ViewControllers::SongListController::PlayerDataLoaded, this};

    SongCore::SongLoader::RuntimeSongLoader::get_instance()->SongsLoaded += {&SongListController::OnSongsLoaded, this};
}

void ViewControllers::SongListController::OnDestroy() {
    DEBUG("SongListController onDestroy");
    // Unsub from events
    dataHolder.loadingFinished -= {&ViewControllers::SongListController::SongDataDone, this};
    dataHolder.loadingFailed -= {&ViewControllers::SongListController::SongDataError, this};
    dataHolder.searchEnded -= {&ViewControllers::SongListController::SearchDone, this};
    dataHolder.playerDataLoaded -= {&ViewControllers::SongListController::PlayerDataLoaded, this};
}

void ViewControllers::SongListController::SelectRandom() {
    DEBUG("SelectRandom");
    auto cellsNumber = this->NumberOfCells();
    if (cellsNumber < 2) {
        return;
    }
    auto id = BetterSongSearch::Util::random(0, cellsNumber - 1);
    songListTable()->SelectCellWithIdx(id, true);
    songListTable()->ScrollToCellWithIdx(id, HMUI::TableView::ScrollPositionType::Beginning, false);
};

void ViewControllers::SongListController::ShowMoreModal() {
    this->moreModal->Show(false, false, nullptr);
    DEBUG("ShowMoreModal");
};

void ViewControllers::SongListController::HideMoreModal() {
    this->moreModal->Hide(false, nullptr);
    DEBUG("HideMoreModal");
};

void ViewControllers::SongListController::ShowCloseConfirmation() {
    this->downloadCancelConfirmModal->Show();
    DEBUG("ShowCloseConfirmation");
};

void ViewControllers::SongListController::ForcedUIClose() {
    fcInstance->ConfirmCancelCallback(true);
    this->downloadCancelConfirmModal->Hide();
};

void ViewControllers::SongListController::ForcedUICloseCancel() {
    fcInstance->ConfirmCancelCallback(false);
    this->downloadCancelConfirmModal->Hide();
};

custom_types::Helpers::Coroutine ViewControllers::SongListController::UpdateDataAndFiltersCoro() {
    // Wait
    co_yield reinterpret_cast<System::Collections::IEnumerator*>(UnityEngine::WaitForSeconds::New_ctor(0.1f));

    // WARNING: There is a bug with bsml update, it runs before the value is changed for some reason
    bool filtersChanged = false;

    FilterTypes::SortMode sort = dataHolder.sort;
    if (selectedSortMode != nullptr) {
        int index = get_sortModeSelections()->IndexOf(static_cast<System::String*>(selectedSortMode.convert()));
        if (index < 0) {
        } else {
            if (index != getPluginConfig().SortMode.GetValue()) {
                filtersChanged = true;
                getPluginConfig().SortMode.SetValue(index);
                sort = (FilterTypes::SortMode) index;
            }
        }
    }

    if (filtersChanged) {
        DEBUG("Sort changed");
        this->SortAndFilterSongs(sort, dataHolder.search, true);
    }
}

void ViewControllers::SongListController::UpdateDataAndFilters() {
    coro(UpdateDataAndFiltersCoro());
    DEBUG("UpdateDataAndFilters");
}

void ViewControllers::SongListController::ShowPlaylistCreation() {
    // Hide modal cause bsml does not support automagic hiding of it
    this->moreModal->Hide(false, nullptr);
    DEBUG("ShowPlaylistCreation");
};

void ViewControllers::SongListController::ShowSettings() {
    this->settingsModal->OpenModal();
    // Hide modal cause bsml does not support automagic hiding of it
    this->moreModal->Hide(false, nullptr);
    DEBUG("ShowSettings");
}

// Song buttons
void ViewControllers::SongListController::Download() {
#ifdef SONGDOWNLOADER
    auto currentSong = GetCurrentSong();
    auto songData = currentSong;

    if (songData != nullptr) {
        fcInstance->DownloadHistoryViewController->TryAddDownload(songData);
        downloadButton->set_interactable(false);
    } else {
        WARNING("Current song is null, doing nothing");
    }
#endif
}

void GetByURLAsync(std::string url, std::function<void(bool success, std::vector<uint8_t>)> finished) {
    std::thread([url, finished] {
        auto response = WebUtils::GetAsync<WebUtils::DataResponse>(WebUtils::URLOptions(url));

        response.wait();

        auto responseValue = response.get();

        bool success = responseValue.IsSuccessful();
        if (!success) {
            DEBUG("Failed to get response for cover image");
            finished(false, {});
            return;
        }
        if (!responseValue.responseData.has_value()) {
            DEBUG("No value in responseData for cover image");
            finished(false, {});
            return;
        }

        auto data = responseValue.responseData.value();

        finished(true, data);
    }).detach();
}

custom_types::Helpers::Coroutine GetPreview(std::string url, std::function<void(UnityW<UnityEngine::AudioClip>)> finished) {
    auto webRequest = UnityEngine::Networking::UnityWebRequestMultimedia::GetAudioClip(url, UnityEngine::AudioType::MPEG);
    co_yield reinterpret_cast<System::Collections::IEnumerator*>(CRASH_UNLESS(webRequest->SendWebRequest()));
    if (webRequest->GetError() != UnityEngine::Networking::UnityWebRequest::UnityWebRequestError::OK) {
        INFO("Network error");
        webRequest->Dispose();  // Cleanup is required for all Unity web requests to prevent weirdness
        finished(nullptr);
        co_return;
    }

    // Wait for download to finish (should probably never happen)
    while (!webRequest->get_isDone()) {
    };

    INFO("Download complete");
    UnityW<UnityEngine::AudioClip> clip = UnityEngine::Networking::DownloadHandlerAudioClip::GetContent(webRequest);
    webRequest->Dispose();  // Cleanup is required for all Unity web requests to prevent weirdness
    finished(clip);
    co_return;
}

void ViewControllers::SongListController::EnterSolo(GlobalNamespace::BeatmapLevel* level) {
    if (level == nullptr) {
        ERROR("Level is null, refusing to continue");
        return;
    }
    fcInstance->Close(true, false);

    auto customLevelsPack = SongCore::API::Loading::GetCustomLevelPack();
    if (customLevelsPack == nullptr) {
        ERROR("CustomLevelsPack is null, refusing to continue");
        return;
    }
    if (customLevelsPack->____beatmapLevels->get_Length() == 0) {
        ERROR("CustomLevelsPack has no levels, refusing to continue");
        return;
    }

    auto category = SelectLevelCategoryViewController::LevelCategory(SelectLevelCategoryViewController::LevelCategory::All);

    // static_assert(sizeof (System::Nullable_1<SelectLevelCategoryViewController::LevelCategory>) == 0x8)
    auto levelCategory = System::Nullable_1<SelectLevelCategoryViewController::LevelCategory>();
    levelCategory.value = category;
    levelCategory.hasValue = true;

    auto state = LevelSelectionFlowCoordinator::State::New_ctor(customLevelsPack, static_cast<GlobalNamespace::BeatmapLevel*>(level));

    state->___levelCategory = levelCategory;

    multiplayerLevelSelectionFlowCoordinator->LevelSelectionFlowCoordinator::Setup(state);
    soloFreePlayFlowCoordinator->Setup(state);

    manager.GoToSongSelect();

    // For some reason setup does not work for multiplayer so I have to use this method to workaround
    if (multiplayerLevelSelectionFlowCoordinator && multiplayerLevelSelectionFlowCoordinator->___levelSelectionNavigationController &&
        multiplayerLevelSelectionFlowCoordinator->___levelSelectionNavigationController->____levelCollectionNavigationController) {
        DEBUG("Selecting level in multiplayer");

        multiplayerLevelSelectionFlowCoordinator->___levelSelectionNavigationController->____levelCollectionNavigationController->SelectLevel(
            static_cast<GlobalNamespace::BeatmapLevel*>(level)
        );
    };
}

void ViewControllers::SongListController::Play() {
    this->PlaySong();
}

void ViewControllers::SongListController::PlaySong(SongDetailsCache::Song const* songToPlay) {
    auto currentSong = GetCurrentSong();
    if (songToPlay == nullptr) {
        songToPlay = currentSong;
        if (currentSong == nullptr) {
            ERROR("Current song is null and songToPlay is null");
            return;
        }
    }

    if (fcInstance->ConfirmCancelOfPending([this, songToPlay]() {
            PlaySong(songToPlay);
        })) {
        return;
    }

    // Grab the song hash and map id, to prevent any issues with threads..
    DEBUG("Song index is: {}", songToPlay->index);
    std::string songHash = songToPlay->hash();
    uint32_t mapId = songToPlay->mapId();

    auto fun = [this, songHash, mapId]() {
        auto level = SongCore::API::Loading::GetLevelByHash(songHash);

        // Fallback for rare cases when the hash is different from the hash in our database (e.g. song got updated)
        if (level == nullptr) {
            // Get song beatsaver id
            std::string songKey = fmt::format("{:X}", mapId);
            songKey = toLower(songKey);
            DEBUG("Looking for level by beatsaver id in path: {}", songKey);
            level = SongCore::API::Loading::GetLevelByFunction([mapId = songKey](auto level) {
                auto levelPath = level->get_customLevelPath();
                return levelPath.find(mapId) != std::string::npos;
            });
        }

        // If all else fails, cancel
        if (level == nullptr) {
            DEBUG("Hash: {}", songHash);
            ERROR("Song somehow is not downloaded and could not find it in our database, pls fix");
            return;
        }

        // If we successfully found the level, we can continue
        BSML::MainThreadScheduler::Schedule([this, level] {
            fromBSS = true;
            openToCustom = true;
            EnterSolo(level);
        });
    };

    if (fcInstance->DownloadHistoryViewController->hasUnloadedDownloads) {
        auto future = SongCore::API::Loading::RefreshSongs(false);
        std::thread(
            [](std::shared_future<void> future, std::function<void()> fun) {
                future.wait();
                fun();
            },
            std::move(future),
            std::move(fun)
        )
            .detach();
    } else {
        fun();
    }
}

void ViewControllers::SongListController::ShowBatchDownload() {
    this->multiDlModal->OpenModal();
    this->HideMoreModal();
}

void ViewControllers::SongListController::ShowSongDetails() {
    auto currentSong = GetCurrentSong();
    if (currentSong) {
        uploadDetailsModal->OpenModal(currentSong);
    }
}

void ViewControllers::SongListController::UpdateDetails() {
    auto currentSong = GetCurrentSong();
    if (currentSong == nullptr) {
        return;
    }

    auto song = currentSong;
    DEBUG("Song index is: {}", song->index);
    auto beatmap = SongCore::API::Loading::GetLevelByHash(std::string(song->hash()));
    bool loaded = beatmap != nullptr;
    bool downloaded = fcInstance->DownloadHistoryViewController->CheckIsDownloaded(std::string(song->hash()));

    float minNPS = 500000, maxNPS = 0;
    float minNJS = 500000, maxNJS = 0;
    for (auto const& diff : *song) {
        float nps = (float) diff.notes / (float) song->songDurationSeconds;
        float njs = diff.njs;
        minNPS = std::min(nps, minNPS);
        maxNPS = std::max(nps, maxNPS);

        minNJS = std::min(njs, minNJS);
        maxNJS = std::max(njs, maxNJS);
    }

    // downloadButton.set.text = "Download";
    SetIsDownloaded(downloaded);
    selectedSongDiffInfo->set_text(fmt::format("{:.2f} - {:.2f} NPS \n {:.2f} - {:.2f} NJS", minNPS, maxNPS, minNJS, maxNJS));
    selectedSongName->set_text(song->songName());
    selectedSongAuthor->set_text(song->songAuthorName());

    // This part below is here to not break anything on return
#ifdef SONGDOWNLOADER

    // if beatmap is loaded
    if (loaded) {
        // Get old sprite if it exists
        UnityW<UnityEngine::Sprite> oldSprite = this->coverImage->get_sprite();

        auto cover = BetterSongSearch::Util::getLocalCoverSync(beatmap);
        if (cover) {
            this->coverImage->set_sprite(cover);

            // Cleanup old sprite
            if (oldSprite && oldSprite.ptr() != defaultImage.ptr()) {
                UnityW<UnityEngine::Texture2D> texture = oldSprite->get_texture();
                if (texture) {
                    UnityEngine::Object::DestroyImmediate(texture);
                }
                UnityEngine::Object::DestroyImmediate(oldSprite);
            }
        } else {
            this->coverImage->set_sprite(defaultImage);

            // Cleanup old sprite
            if (oldSprite && oldSprite.ptr() != defaultImage.ptr()) {
                UnityW<UnityEngine::Texture2D> texture = oldSprite->get_texture();
                if (texture) {
                    UnityEngine::Object::DestroyImmediate(texture);
                }
                UnityEngine::Object::DestroyImmediate(oldSprite);
            }
        }
        coverLoading->set_enabled(false);
    } else {
        std::string newUrl = fmt::format("{}/{}.jpg", BeatSaverRegionManager::coverDownloadUrl, toLower(song->hash()));
        DEBUG("{}", newUrl.c_str());
        coverLoading->set_enabled(true);
        GetByURLAsync(newUrl, [this, song](bool success, std::vector<uint8_t> bytes) {
            BSML::MainThreadScheduler::Schedule([this, bytes, song, success] {
                // Get old sprite if it exists
                UnityW<UnityEngine::Sprite> oldSprite = this->coverImage->get_sprite();
                if (success) {
                    std::vector<uint8_t> data = bytes;
                    DEBUG("Image size: {}", pretty_bytes(bytes.size()));
                    auto currentSong = GetCurrentSong();
                    // Return if the song has changed somehow
                    if (currentSong == nullptr) {
                        return;
                    }
                    if (song->hash() != currentSong->hash()) {
                        DEBUG("Song hash changed, returning");
                        return;
                    }
                    auto spriteArray = ArrayW(data);
                    UnityW<UnityEngine::Sprite> sprite = BSML::Lite::ArrayToSprite(spriteArray);
                    if (sprite) {
                        DEBUG("Setting sprite");
                        this->coverImage->set_sprite(sprite);
                    } else {
                        WARNING("Setting default image, sprite was invalid");
                        this->coverImage->set_sprite(defaultImage);
                    }
                } else {
                    this->coverImage->set_sprite(defaultImage);
                }

                // Disable loading animation
                coverLoading->set_enabled(false);

                // Cleanup old sprite
                if (oldSprite && oldSprite.ptr() != defaultImage.ptr() &&  // Old sprite is not default image
                    this->coverImage->get_sprite().ptr() != oldSprite.ptr()  // Old sprite is not the current sprite
                ) {
                    UnityW<UnityEngine::Texture2D> texture = oldSprite->get_texture();
                    if (texture) {
                        UnityEngine::Object::DestroyImmediate(texture);
                    }
                    UnityEngine::Object::DestroyImmediate(oldSprite);
                }
            });
        });
    }

    // If the song is loaded then get it from local sources
    if (loaded) {
        if (!levelCollectionViewController || levelCollectionViewController->m_CachedPtr.m_value == nullptr) {
            return;
        }

        levelCollectionViewController->SongPlayerCrossfadeToLevelAsync(beatmap, System::Threading::CancellationToken::get_None());
    } else {
        if (!getPluginConfig().LoadSongPreviews.GetValue()) {
        } else {
            if (!songPreviewPlayer || songPreviewPlayer->m_CachedPtr.m_value == nullptr) {
                return;
            }

            std::string newUrl = fmt::format("{}/{}.mp3", BeatSaverRegionManager::coverDownloadUrl, toLower(song->hash()));

            coro(GetPreview(
                newUrl,

                std::function<void(UnityW<UnityEngine::AudioClip>)>([this, song](UnityW<UnityEngine::AudioClip> clip) {
                    if (!clip) {
                        return;
                    }
                    // Get current song
                    auto currentSong = GetCurrentSong();
                    // Return if the song has changed somehow
                    if (currentSong == nullptr) {
                        UnityEngine::Object::Destroy(clip);
                        return;
                    }
                    if (song->hash() != currentSong->hash()) {
                        DEBUG("Song hash changed, returning");
                        UnityEngine::Object::Destroy(clip);
                        return;
                    }

                    // Audio clip cleanup
                    std::function<void()> onFadeOutLambda = [clip]() {
                        try {
                            if (clip) {
                                UnityEngine::Object::Destroy(clip);
                            }
                        } catch (...) {
                            ERROR("Error destroying clip");
                        }
                    };
                    auto onFadeOut = BSML::MakeDelegate<System::Action*>(onFadeOutLambda);

                    songPreviewPlayer->CrossfadeTo(clip, -5, 0, clip->get_length(), onFadeOut);
                })
            ));
        }
    }

#endif
}

void ViewControllers::SongListController::FilterByUploader() {
    auto currentSong = GetCurrentSong();
    if (!currentSong) {
        WARNING("currentSong is null, returning");
        return;
    }

    fcInstance->FilterViewController->uploadersString = currentSong->uploaderName();
    SetStringSettingValue(fcInstance->FilterViewController->uploadersStringControl, (std::string) currentSong->uploaderName());
    fcInstance->FilterViewController->UpdateFilterSettings();
    DEBUG("FilterByUploader");
}

// BSML::CustomCellInfo
HMUI::TableCell* ViewControllers::SongListController::CellForIdx(HMUI::TableView* tableView, int idx) {
    auto song = dataHolder.GetDisplayedSongByIndex(idx);
    return ViewControllers::SongListTableData::GetCell(tableView)->PopulateWithSongData(song);
}

void ViewControllers::SongListController::UpdateSearch() {
}

void ViewControllers::SongListController::SortAndFilterSongs(FilterTypes::SortMode sort, std::string_view const search, bool resetTable) {
    // Skip if not active
    if (!get_isActiveAndEnabled()) {
        return;
    }

    dataHolder.sort = sort;
    dataHolder.search = search;

    this->UpdateSearchedSongsList();
}

void ViewControllers::SongListController::SetSelectedSong(SongDetailsCache::Song const* song) {
    // TODO: Fill all fields, download image, activate buttons
    auto prevSong = GetCurrentSong();
    if (prevSong != nullptr && song != nullptr && prevSong->hash() == song->hash()) {
        return;
    }

    SetCurrentSong(song);

    DEBUG("Updating details");
    this->UpdateDetails();
}

void ViewControllers::SongListController::SetIsDownloaded(bool isDownloaded, bool downloadable) {
    playButton->get_gameObject()->set_active(isDownloaded);
    playButton->set_interactable(isDownloaded);
    downloadButton->get_gameObject()->set_active(!isDownloaded);
    downloadButton->set_interactable(!isDownloaded);
    infoButton->set_interactable(true);
}

void ViewControllers::SongListController::DownloadSongList() {
    fcInstance->FilterViewController->datasetInfoLabel->set_text("Loading dataset...");
    dataHolder.DownloadSongList();
}

void ViewControllers::SongListController::RetryDownloadSongList() {
    if (dataHolder.failed && !dataHolder.loading && !dataHolder.loaded) {
        this->DownloadSongList();
    }
}

void ViewControllers::SongListController::SearchDone() {
    auto isMainThread = BSML::MainThreadScheduler::CurrentThreadIsMainThread();
    if (!isMainThread) {
        ERROR("Calling SearchDone not on the main thread is not allowed, returning");
        return;
    }

    DEBUG("Displaying {} songs", dataHolder.GetDisplayedSongListLength());
    if (!songListTable()) {
        // TODO: Actually understand why songListTable isn't available on soft refresh
        WARNING("SongListTable is null, might be a soft refresh, returning as we don't need to reset anything on soft refresh");
        return;
    }

    long long before = 0;
    before = CurrentTimeMs();
    this->ResetTable();
    INFO("table reset in {} ms", CurrentTimeMs() - before);

    if (songSearchPlaceholder) {
        if (dataHolder.GetDisplayedSongListLength() == dataHolder.songDetails->songs.size()) {
            songSearchPlaceholder->set_text("Search by Song, Key, Mapper..");
        } else {
            songSearchPlaceholder->set_text(fmt::format("Search {} songs", dataHolder.GetDisplayedSongListLength()));
        }
    }

    auto currentSong = GetCurrentSong();
    // Reset song list table selection
    if (!currentSong) {
        SelectSong(songListTable(), 0);
    } else {
        // Always un-select in the list to prevent wrong-selections on resorting, etc.
        songListTable()->ClearSelection();
    }

    this->searchInProgress->get_gameObject()->set_active(false);

    // Run search again if something wants to
    bool currentSortChanged = dataHolder.currentSort != dataHolder.sort;
    bool currentSearchChanged = dataHolder.currentSearch != dataHolder.search;
    bool currentFilterChanged = !dataHolder.filterOptionsCache.IsEqual(dataHolder.filterOptions);

    IsSearching = false;

    // Queue another search at the end of this one
    if (currentSearchChanged || currentSortChanged || currentFilterChanged) {
        DEBUG("Queueing another search");
        DEBUG("Sort: {}, Search: {}, Filter: {}", currentSortChanged, currentSearchChanged, currentFilterChanged);
        this->UpdateSearchedSongsList();
    }
}

// Event receivers
void ViewControllers::SongListController::SongDataDone() {
    BSML::MainThreadScheduler::Schedule([this] {
        if (this->get_isActiveAndEnabled()) {
            dataHolder.needsRefresh = false;
            // TODO: Move into dataholder
            // Initial search
            dataHolder.forceReload = true;
            fcInstance->SongListController->SortAndFilterSongs(dataHolder.sort, dataHolder.search, true);
        }
    });
}

void ViewControllers::SongListController::SongDataError(std::string message) {
}

void ViewControllers::SongListController::PlayerDataLoaded() {
    if (!songListTable()) {
        return;
    }
    // If it's searching, don't reload, it will be done after search
    if (dataHolder.searchInProgress) {
        return;
    }

    // IDK why this is here, but it's here
}

void ViewControllers::SongListController::OnSongsLoaded(std::span<SongCore::SongLoader::CustomBeatmapLevel* const> songs) {
    auto currentSong = GetCurrentSong();
    if (dataHolder.songDetails == nullptr) {
        return;
    }
    if (!dataHolder.songDetails->songs.get_isDataAvailable()) {
        return;
    }
    if (currentSong == nullptr) {
        return;
    }

    // Ensure it runs on the main thread
    bool isMainThread = BSML::MainThreadScheduler::CurrentThreadIsMainThread();
    if (!isMainThread) {
        ERROR("Calling OnSongsLoaded not on the main thread, sending to main thread");
        BSML::MainThreadScheduler::Schedule([this, songs] {
            this->OnSongsLoaded(songs);
        });
        return;
    }

    auto song = currentSong;
    DEBUG("Song index is: {}", song->index);
    auto beatmap = SongCore::API::Loading::GetLevelByHash(std::string(song->hash()));
    bool loaded = beatmap != nullptr;

    SetIsDownloaded(loaded);
}

SongDetailsCache::Song const* ViewControllers::SongListController::GetCurrentSong() {
    std::shared_lock<std::shared_mutex> lock(_currentSongMutex);
    return _currentSong;
}

void ViewControllers::SongListController::SetCurrentSong(SongDetailsCache::Song const* song) {
    std::unique_lock<std::shared_mutex> lock(_currentSongMutex);
    _currentSong = song;
}
