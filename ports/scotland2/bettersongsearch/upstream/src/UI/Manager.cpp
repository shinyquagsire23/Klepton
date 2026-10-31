#include "UI/Manager.hpp"

#include <bsml/shared/Helpers/getters.hpp>

#include "DataHolder.hpp"
#include "GlobalNamespace/MenuTransitionsHelper.hpp"
#include "HMUI/NoTransitionsButton.hpp"
#include "logging.hpp"
#include "sys/sysinfo.h"
#include "sys/types.h"
#include "System/GC.hpp"
#include "UnityEngine/Profiling/Profiler.hpp"
#include "UnityEngine/Resources.hpp"
#include "Util/Debug.hpp"

using namespace BetterSongSearch::UI;
using namespace BetterSongSearch::Util;
using namespace GlobalNamespace;

#define coro(coroutine) BSML::SharedCoroutineStarter::get_instance()->StartCoroutine(custom_types::Helpers::CoroutineHelper::New(coroutine))

void BetterSongSearch::UI::Manager::Init() {
    // Register the menu button
    BSML::Register::RegisterMenuButton("Better Song Search", "Search songs, but better", [this]() {
        DEBUG("MenuButtonClick");
        ShowFlow(false);
    });
}

custom_types::Helpers::Coroutine BetterSongSearch::UI::Manager::Debug() {
    static struct sysinfo memInfo;
    int iterations_count = 0;
    int64_t lastused = 0;
    int64_t initialmemusage = UnityEngine::Profiling::Profiler::GetMonoUsedSizeLong();
    // wait for the game to start
    co_yield reinterpret_cast<System::Collections::IEnumerator*>(UnityEngine::WaitForSeconds::New_ctor(3.0f));

    // Open the BSS
    ShowFlow(true);
    co_yield reinterpret_cast<System::Collections::IEnumerator*>(UnityEngine::WaitForSeconds::New_ctor(1.0f));

    // Wait for songs to load
    while (!dataHolder.loaded) {
        co_yield reinterpret_cast<System::Collections::IEnumerator*>(UnityEngine::WaitForSeconds::New_ctor(0.2f));
    }

    // co_return;

    while (true) {
        // Tests for the fcinstance
        if (fcInstance) {
            DEBUG("Iteration: {}", iterations_count);
            //     // Select random song go into the play menu and go back
            {
                auto songlist = fcInstance->SongListController;
                co_yield reinterpret_cast<System::Collections::IEnumerator*>(UnityEngine::WaitForSeconds::New_ctor(0.01f));
                songlist->SelectRandom();
                auto currentSong = songlist->GetCurrentSong();
                if (currentSong) {
                    // co_yield reinterpret_cast<System::Collections::IEnumerator*>(UnityEngine::WaitForSeconds::New_ctor(0.1f));

                    // // Play song and go back
                    // if (songlist->playButton->get_gameObject()->get_active()) {
                    //     songlist->PlaySong(currentSong);
                    //     co_yield reinterpret_cast<System::Collections::IEnumerator*>(UnityEngine::WaitForSeconds::New_ctor(2.5f));
                    //     // Press back button
                    //     UnityEngine::GameObject::Find("BackButton")->GetComponent<HMUI::NoTransitionsButton*>()->Press();
                    // }

                    ++iterations_count;
                } else {
                    WARNING("Current song is null, skipping");
                }
            }

            // Pick random song, show details, close details
            // {
            //     auto songlist = fcInstance->SongListController;
            //     co_yield reinterpret_cast<System::Collections::IEnumerator*>(UnityEngine::WaitForSeconds::New_ctor(0.1f));
            //     songlist->SelectRandom();
            //     auto currentSong = songlist->GetCurrentSong();
            //     if (currentSong) {
            //         co_yield reinterpret_cast<System::Collections::IEnumerator*>(UnityEngine::WaitForSeconds::New_ctor(0.1f));
            //         songlist->ShowSongDetails();
            //         co_yield reinterpret_cast<System::Collections::IEnumerator*>(UnityEngine::WaitForSeconds::New_ctor(0.1f));
            //         songlist->uploadDetailsModal->CloseModal();
            //         ++iterations_count;
            //     }
            // }
        }

        // Debug restart of the game
        // {
        //     DEBUG("Open BSS");

        //     DEBUG("Press back button");
        //     // Press back button
        //     UnityEngine::GameObject::Find("BackButton")->GetComponent<HMUI::NoTransitionsButton*>()->Press();

        //     co_yield reinterpret_cast<System::Collections::IEnumerator*>(UnityEngine::WaitForSeconds::New_ctor(3.0f));

        //     DEBUG("Reload the game manually");
        //     // Reload the game manually
        //     UnityEngine::Resources::FindObjectsOfTypeAll<MenuTransitionsHelper*>()[0]->RestartGame(nullptr);
        //     co_yield reinterpret_cast<System::Collections::IEnumerator*>(UnityEngine::WaitForSeconds::New_ctor(5.0f));
        // }

        // Select current song, go into song list and back
        // {
        //     co_yield reinterpret_cast<System::Collections::IEnumerator*>(UnityEngine::WaitForSeconds::New_ctor(0.5f));
        //     // this->SelectRandom();
        //     if (this->currentSong) {
        //         co_yield reinterpret_cast<System::Collections::IEnumerator*>(UnityEngine::WaitForSeconds::New_ctor(0.5f));
        //         this->PlaySong(this->currentSong);
        //         co_yield reinterpret_cast<System::Collections::IEnumerator*>(UnityEngine::WaitForSeconds::New_ctor(1.5f));

        //         // Press back button
        //         UnityEngine::GameObject::Find("BackButton")->GetComponent<HMUI::NoTransitionsButton*>()->Press();
        //         // manager.ShowFlow(true);

        //         ++iterations_count;
        //     }
        // }

        // GC testing
        // {
        //     System::GC::Collect();

        //     int64_t mem = UnityEngine::Profiling::Profiler::GetMonoUsedSizeLong();
        //     DEBUG(
        //         "\nIter {}\nUsed {}\nChan {}\nInit: {}",
        //         iterations_count,
        //         pretty_bytes(mem),
        //         pretty_bytes(mem - lastused),
        //         pretty_bytes(lastused - initialmemusage)
        //     );
        //     lastused = mem;
        // }
    }
    co_yield nullptr;
}

void BetterSongSearch::UI::Manager::DestroyFlow() {
    if (flow) {
        DEBUG("Destroying BSS flowController");
        auto flowGO = flow->get_gameObject();
        if (flowGO) {
            UnityEngine::Object::DestroyImmediate(flowGO);
        }
    } else {
        WARNING("Destroy flow called when the controller didn't exist");
    }
}

void BetterSongSearch::UI::Manager::ShowFlow(bool immediately) {
    if (!flow) {
        flow = BSML::Helpers::CreateFlowCoordinator<BetterSongSearch::UI::FlowCoordinators::BetterSongSearchFlowCoordinator*>();
    }
    parentFlow = BSML::Helpers::GetMainFlowCoordinator()->YoungestChildFlowCoordinatorOrSelf();
    // TODO: Recover in case parentFlow is null
    parentFlow->PresentFlowCoordinator(flow.ptr(), nullptr, HMUI::ViewController::AnimationDirection::Horizontal, false, false);
}

void BetterSongSearch::UI::Manager::GoToSongSelect() {
    SafePtrUnity<UnityEngine::GameObject> songSelectButton = UnityEngine::GameObject::Find("SoloButton").unsafePtr();
    if (!songSelectButton) {
        songSelectButton = UnityEngine::GameObject::Find("Wrapper/BeatmapWithModifiers/BeatmapSelection/EditButton");
    }
    if (!songSelectButton) {
        return;
    }
    songSelectButton->GetComponent<HMUI::NoTransitionsButton*>()->Press();
}

void BetterSongSearch::UI::Manager::Close(bool immediately, bool downloadAbortConfim) {
    if (flow) {
        flow->Close(immediately, downloadAbortConfim);
    }
};
