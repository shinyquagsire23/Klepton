// Diagnostic only: call the actual private SongCore collector with its exact
// NDK types. -fno-access-control changes access checking, never object layouts.
#include "SongLoader/RuntimeSongLoader.hpp"
#include <cstdio>
#include <thread>
static int collect(const char *root) {
    using Loader=SongCore::SongLoader::RuntimeSongLoader;
    for(unsigned pass=0;pass<100;++pass) {
        std::set<Loader::LevelPathAndWip> paths;
        Loader::CollectLevels(std::filesystem::path(root),false,paths);
        if(paths.size()!=3){std::fprintf(stderr,"SONGCORE_COLLECT_COUNT pass=%u count=%zu\n",pass,paths.size());return 1;}
    }
    std::fprintf(stderr,"SONGCORE_COLLECT_FIXTURE_OK 100 original CollectLevels calls with nested Info.dat directories and autosaves\n");
    return 0;
}

extern "C" __attribute__((visibility("default")))
int sc2_test_recursive_filesystem(const char *root) {
    int result=collect(root);if(result)return result;
    std::thread worker([&]{result=collect(root);});worker.join();
    return result;
}
