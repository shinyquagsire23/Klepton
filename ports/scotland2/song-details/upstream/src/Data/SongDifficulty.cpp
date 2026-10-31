#include "Data/SongDifficulty.hpp"
#include "Data/SongDetailsContainer.hpp"
#include "Data/Song.hpp"
#include "SongProto.pb.h"

namespace SongDetailsCache {
    const SongDifficulty SongDifficulty::none{0, nullptr};
    SongDifficulty::SongDifficulty(std::size_t songIndex, const Structs::SongDifficulty* proto) noexcept :
        songIndex(songIndex),
        characteristic(proto && proto->has_characteristic() ? static_cast<MapCharacteristic>(proto->characteristic()) : MapCharacteristic::Standard),
        difficulty(proto && proto->has_difficulty() ? static_cast<MapDifficulty>(proto->difficulty()) : MapDifficulty::ExpertPlus),
        starsSS(proto && proto->has_starst100() ? proto->starst100() / 100.0f : 0),
        starsBL(proto && proto->has_starst100bl() ? proto->starst100bl() / 100.0f : 0),
        njs(proto ? proto->njst100() / 100.0f : 0),
        bombs(proto && proto->has_bombs() ? proto->bombs() : 0),
        notes(proto && proto->has_notes() ? proto->notes() : 0),
        obstacles(proto && proto->has_obstacles() ? proto->obstacles() : 0),
        mods(proto && proto->has_mods() ? static_cast<MapMods>(proto->mods()) : MapMods::None)
        {}

    const Song& SongDifficulty::song() const noexcept {
        return SongDetailsContainer::songs->operator[](songIndex);
    }

    bool SongDifficulty::ranked() const noexcept {
        return this->rankedSS() || this->rankedBL();
    }

    bool SongDifficulty::rankedBL() const noexcept {
        return starsBL > 0 && hasFlags(song().rankedStates, RankedStates::BeatleaderRanked);
    }

    bool SongDifficulty::rankedSS() const noexcept {
        return starsSS > 0 && hasFlags(song().rankedStates, RankedStates::ScoresaberRanked);
    }

    bool SongDifficulty::usesMods(const MapMods& usedMods) const noexcept {
        return hasFlags(mods, usedMods);
    }
}