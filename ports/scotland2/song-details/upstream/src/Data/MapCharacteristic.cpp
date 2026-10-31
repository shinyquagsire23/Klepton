#include "Data/MapCharacteristic.hpp"

namespace SongDetailsCache {
    const auto CustomHash = std::hash<std::string>{}("Custom");
    const auto StandardHash = std::hash<std::string>{}("Standard");
    const auto OneSaberHash = std::hash<std::string>{}("OneSaber");
    const auto NoArrowsHash = std::hash<std::string>{}("NoArrows");
    const auto NinetyDegreeHash = std::hash<std::string>{}("NinetyDegree");
    const auto ThreeSixtyDegreeHash = std::hash<std::string>{}("ThreeSixtyDegree");
    const auto LightShowHash = std::hash<std::string>{}("LightShow");
    const auto LawlessHash = std::hash<std::string>{}("Lawless");

    bool parse(std::string_view str, MapCharacteristic& out) {
        auto h = std::hash<std::string_view>{}(str);
        if (h == CustomHash) { out = MapCharacteristic::Custom; return true; }
        if (h == StandardHash) { out = MapCharacteristic::Standard; return true; }
        if (h == OneSaberHash) { out = MapCharacteristic::OneSaber; return true; }
        if (h == NoArrowsHash) { out = MapCharacteristic::NoArrows; return true; }
        if (h == NinetyDegreeHash) { out = MapCharacteristic::NinetyDegree; return true; }
        if (h == ThreeSixtyDegreeHash) { out = MapCharacteristic::ThreeSixtyDegree; return true; }
        if (h == LightShowHash) { out = MapCharacteristic::LightShow; return true; }
        if (h == LawlessHash) { out = MapCharacteristic::Lawless; return true; }
        return false;
    }
}