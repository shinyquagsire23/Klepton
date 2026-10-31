// Observe the actual SongCore service and scan through its public APIs.
#include "SongCore.hpp"
#include "SongLoader/RuntimeSongLoader.hpp"
#include "bsml/shared/BSML/MainThreadScheduler.hpp"
#include <cstdio>
extern "C" __attribute__((visibility("default")))
int sc2_test_songcore_lifecycle() {
    BSML::MainThreadScheduler::ScheduleUntil([] {
        return SongCore::SongLoader::RuntimeSongLoader::get_instance()!=nullptr;
    },[] {
        std::fprintf(stderr,"[scotland2] SONGCORE_SERVICE_OK actual Zenject RuntimeSongLoader initialized\n");
    });
    BSML::MainThreadScheduler::ScheduleUntil([] {
        return SongCore::API::Loading::AreSongsLoaded() && !SongCore::API::Loading::AreSongsRefreshing();
    },[] {
        auto levels=SongCore::API::Loading::GetAllLevels();
        std::fprintf(stderr,"[scotland2] SONGCORE_SONG_SCAN_OK actual SongCore finished scanning: %zu custom levels; playback not tested\n",levels.size());
    });
    return 0;
}

#include "BGLib/DotnetExtension/Collections/LRUCache_2.hpp"
#include "UnityEngine/Sprite.hpp"
extern "C" __attribute__((visibility("default")))
void *sc2_songcore_generic_target() {
    using Cache=BGLib::DotnetExtension::Collections::LRUCache_2<StringW,UnityEngine::Sprite*>;
    auto method=il2cpp_utils::FindMethodUnsafe(classof(Cache*),"Add",2);
    return method?reinterpret_cast<void*>(method->methodPointer):nullptr;
}
