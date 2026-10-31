#include "SongLoader/CustomBeatmapLevelsRepository.hpp"
#include "System/Collections/Generic/Dictionary_2.hpp"
#include "logging.hpp"

DEFINE_TYPE(SongCore::SongLoader, CustomBeatmapLevelsRepository);

namespace SongCore::SongLoader {
    void CustomBeatmapLevelsRepository::ctor() {
        INVOKE_CTOR();
        _levelPacks = ListW<GlobalNamespace::BeatmapLevelPack*>::New();
        _ctor(_levelPacks->i___System__Collections__Generic__IEnumerable_1_T_());
    }

    void CustomBeatmapLevelsRepository::AddLevelPack(GlobalNamespace::BeatmapLevelPack* pack) {
        auto itr = std::find_if(_levelPacks.begin(), _levelPacks.end(), [pack](auto x){ return x->packID == pack->packID; });
        if (itr == _levelPacks.end()) {
            _levelPacks->Add(pack);
        } else {
            WARNING("A pack with id {} was already added, not adding again!", pack->packID);
        }
    }

    void CustomBeatmapLevelsRepository::RemoveLevelPack(GlobalNamespace::BeatmapLevelPack* pack) {
        auto itr = std::find_if(_levelPacks.begin(), _levelPacks.end(), [pack](auto x){ return x->packID == pack->packID; });
        if (itr != _levelPacks.end()) {
            _levelPacks->Remove(*itr);
        } else {
            WARNING("A pack with id {} was not added, not removing!", pack->packID);
        }
    }

    void CustomBeatmapLevelsRepository::ClearLevelPacks() {
        _levelPacks.clear();
        _idToBeatmapLevelPack->Clear();
        _beatmapLevelIdToBeatmapLevelPackId->Clear();
        _idToBeatmapLevel->Clear();
    }

    void CustomBeatmapLevelsRepository::FixBackingDictionaries() {
        _beatmapLevelPacks = _levelPacks->ToArray();
        _idToBeatmapLevelPack->Clear();
        _beatmapLevelIdToBeatmapLevelPackId->Clear();
        _idToBeatmapLevel->Clear();

        // for every pack
        for (auto pack : _levelPacks) {
            auto packID = pack->packID;
            _idToBeatmapLevelPack->TryAdd(packID, pack);

            // for every level
            for (auto level : pack->_beatmapLevels) {
                auto levelID = level->levelID;

                _beatmapLevelIdToBeatmapLevelPackId->TryAdd(levelID, packID);
                _idToBeatmapLevel->TryAdd(levelID, level);
            }
        }
    }
}
