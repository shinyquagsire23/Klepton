#include "FilterOptions.hpp"

#include "DataHolder.hpp"
#include "logging.hpp"
#include "main.hpp"
#include "Util/TextUtil.hpp"

using namespace rapidjson;
using namespace UnityEngine;
using namespace BetterSongSearch::Util;

bool BetterSongSearch::FilterProfile::IsDefault() {
    // clang-format off
    return (
        downloadType == (int)FilterTypes::DownloadFilter::All &&
        localScoreType ==  (int)FilterTypes::LocalScoreFilter::All &&
        (maxLength / 60 >= SONG_LENGTH_FILTER_MAX) &&
        (minLength == 0) &&
        minNJS == 0 &&
        maxNJS >= NJS_FILTER_MAX &&
        minNPS == 0 &&
        maxNPS >= NPS_FILTER_MAX &&
        rankedType == (int)FilterTypes::RankedFilter::ShowAll &&
        minStars == 0 &&
        maxStars >= STAR_FILTER_MAX &&
        minUploadDate == BEATSAVER_EPOCH &&
        minUploadDateInMonths == 0 &&
        minRating == 0 &&
        minVotes == 0 &&
        uploaders.empty() &&
        difficultyFilter == (int)FilterTypes::DifficultyFilter::All &&
        charFilter == (int)FilterTypes::CharFilter::All &&
        modRequirement == (int)FilterTypes::Requirement::Any &&
        !onlyCuratedMaps &&
        !onlyVerifiedMappers &&
        !onlyV3Maps &&
        mapStyleString == "All" &&
        mapGenreString == "" &&
        mapGenreExcludeString == ""
    );
    // clang-format on
}

void BetterSongSearch::FilterProfile::LoadFromConfig() {
    downloadType = getPluginConfig().DownloadType.GetValue();
    localScoreType = getPluginConfig().LocalScoreType.GetValue();
    minLength = getPluginConfig().MinLength.GetValue();
    maxLength = getPluginConfig().MaxLength.GetValue();
    minNJS = getPluginConfig().MinNJS.GetValue();
    maxNJS = getPluginConfig().MaxNJS.GetValue();
    minNPS = getPluginConfig().MinNPS.GetValue();
    maxNPS = getPluginConfig().MaxNPS.GetValue();
    rankedType = getPluginConfig().RankedType.GetValue();
    minStars = getPluginConfig().MinStars.GetValue();
    maxStars = getPluginConfig().MaxStars.GetValue();
    minUploadDate = getPluginConfig().MinUploadDate.GetValue();
    minRating = getPluginConfig().MinRating.GetValue();
    minVotes = getPluginConfig().MinVotes.GetValue();
    charFilter = getPluginConfig().CharacteristicType.GetValue();
    difficultyFilter = getPluginConfig().DifficultyType.GetValue();
    modRequirement = getPluginConfig().RequirementType.GetValue();
    minUploadDateInMonths = getPluginConfig().MinUploadDateInMonths.GetValue();
    onlyCuratedMaps = getPluginConfig().OnlyCuratedMaps.GetValue();
    onlyVerifiedMappers = getPluginConfig().OnlyVerifiedMappers.GetValue();
    onlyV3Maps = getPluginConfig().OnlyV3Maps.GetValue();
    mapStyleString = getPluginConfig().MapStyleString.GetValue();
    mapGenreString = getPluginConfig().MapGenreString.GetValue();
    mapGenreExcludeString = getPluginConfig().MapGenreExcludeString.GetValue();

    // Custom string loader
    auto uploadersString = getPluginConfig().Uploaders.GetValue();
    if (!uploadersString.empty()) {
        if (uploadersString[0] == '!') {
            uploadersString.erase(0, 1);
            uploadersBlackList = true;
        } else {
            uploadersBlackList = false;
        }
        uploaders = split(toLower(uploadersString), " ");
    } else {
        uploaders.clear();
    }
}

void BetterSongSearch::FilterProfile::SaveToConfig() {
    getPluginConfig().DownloadType.SetValue((int) downloadType, false);
    getPluginConfig().LocalScoreType.SetValue((int) localScoreType, false);
    getPluginConfig().MinLength.SetValue(minLength, false);
    getPluginConfig().MaxLength.SetValue(maxLength, false);
    getPluginConfig().MinNJS.SetValue(minNJS, false);
    getPluginConfig().MaxNJS.SetValue(maxNJS, false);
    getPluginConfig().MinNPS.SetValue(minNPS, false);
    getPluginConfig().MaxNPS.SetValue(maxNPS, false);
    getPluginConfig().RankedType.SetValue((int) rankedType, false);
    getPluginConfig().MinStars.SetValue(minStars, false);
    getPluginConfig().MaxStars.SetValue(maxStars, false);
    getPluginConfig().MinUploadDate.SetValue(minUploadDate, false);
    getPluginConfig().MinRating.SetValue(minRating, false);
    getPluginConfig().MinVotes.SetValue(minVotes, false);
    getPluginConfig().CharacteristicType.SetValue((int) charFilter, false);
    getPluginConfig().DifficultyType.SetValue((int) difficultyFilter, false);
    getPluginConfig().RequirementType.SetValue((int) modRequirement, false);
    getPluginConfig().MinUploadDateInMonths.SetValue(minUploadDateInMonths, false);
    getPluginConfig().OnlyCuratedMaps.SetValue(onlyCuratedMaps, false);
    getPluginConfig().OnlyVerifiedMappers.SetValue(onlyVerifiedMappers, false);
    getPluginConfig().OnlyV3Maps.SetValue(onlyV3Maps, false);
    getPluginConfig().MapStyleString.SetValue(mapStyleString, false);
    getPluginConfig().MapGenreString.SetValue(mapGenreString, false);
    getPluginConfig().MapGenreExcludeString.SetValue(mapGenreExcludeString, false);

    getPluginConfig().Uploaders.SetValue((uploadersBlackList ? "!" : "") + join(uploaders, " "), false);

    getPluginConfig().Save();
}

void BetterSongSearch::FilterProfile::CopyFrom(FilterProfile const& other) {
    downloadType = other.downloadType;
    localScoreType = other.localScoreType;
    minLength = other.minLength;
    maxLength = other.maxLength;
    minNJS = other.minNJS;
    maxNJS = other.maxNJS;
    minNPS = other.minNPS;
    maxNPS = other.maxNPS;
    rankedType = other.rankedType;
    minStars = other.minStars;
    maxStars = other.maxStars;
    minUploadDate = other.minUploadDate;
    minRating = other.minRating;
    minVotes = other.minVotes;
    charFilter = other.charFilter;
    difficultyFilter = other.difficultyFilter;
    modRequirement = other.modRequirement;
    minUploadDateInMonths = other.minUploadDateInMonths;
    onlyCuratedMaps = other.onlyCuratedMaps;
    onlyVerifiedMappers = other.onlyVerifiedMappers;
    onlyV3Maps = other.onlyV3Maps;
    mapStyleString = other.mapStyleString;
    mapGenreString = other.mapGenreString;
    mapGenreExcludeString = other.mapGenreExcludeString;
    uploaders = other.uploaders;
    uploadersBlackList = other.uploadersBlackList;
}

std::optional<BetterSongSearch::FilterProfile> BetterSongSearch::FilterProfile::LoadFromPreset(std::string presetName) {
    std::string presetsDir = getDataDir(modInfo) + "/Presets/";

    // Ensure the directory exists
    if (!direxists(presetsDir)) {
        mkpath(presetsDir);
    }

    std::string path = presetsDir + presetName + ".json";

    if (!fileexists(path)) {
        return std::nullopt;
    }

    try {
        DEBUG("Loading preset: {}", presetName);
        auto preset = ReadFromFile<FilterProfile>(path);
        preset.PrintToDebug();
        DEBUG("Loaded preset: {}", presetName);
        return preset;
    } catch (std::exception const& e) {
        ERROR("Failed to load preset: {}", e.what());
        return std::nullopt;
    }
}

bool BetterSongSearch::FilterProfile::SaveToPreset(std::string presetName) const {
    std::string presetsDir = getDataDir(modInfo) + "/Presets/";

    // Ensure the directory exists
    if (!direxists(presetsDir)) {
        mkpath(presetsDir);
    }

    std::string path = presetsDir + presetName + ".json";

    try {
        DEBUG("Saving preset: {}", presetName);
        WriteToFile<FilterProfile>(path, *this);
        DEBUG("Saved preset: {}", presetName);
        return true;
    } catch (...) {
        ERROR("Failed to save preset: {}", presetName);
        return false;
    }
}

std::vector<std::string> BetterSongSearch::FilterProfile::GetPresetList() {
    std::vector<std::string> presetNames;

    std::string presetsDir = getDataDir(modInfo) + "/Presets/";

    // Ensure the directory exists
    if (!direxists(presetsDir)) {
        mkpath(presetsDir);
    }

    if (!std::filesystem::is_directory(presetsDir)) {
        return presetNames;
    }

    std::error_code ec;
    auto directory_iterator = std::filesystem::directory_iterator(presetsDir, std::filesystem::directory_options::none, ec);
    for (auto const& entry : directory_iterator) {
        if (!entry.is_regular_file()) {
            continue;
        }
        std::string file_extension = entry.path().extension().string();
        std::string raw_file_name = entry.path().filename().replace_extension().string();
        if (file_extension == ".json") {
            presetNames.push_back(raw_file_name);
        }
    }
    std::sort(presetNames.begin(), presetNames.end(), [](std::string& a, std::string& b) {
        return a < b;
    });

    return presetNames;
}

static uint64_t CalculateTagsBitfield(std::string tags) {
    if (tags == "" || BetterSongSearch::dataHolder.songDetails == nullptr) {
        return 0;
    }

    std::vector<std::string> items = split(toLower(tags), " ");

    // Remove empty strings
    items.erase(
        std::remove_if(
            items.begin(),
            items.end(),
            [](std::string const& s) {
                return s.empty();
            }
        ),
        items.end()
    );

    uint64_t res = 0;

    for (auto& t : items) {
        // If it doesnt exist, the default for numbers is 0, and when we add that to an int nothing changes lohl
        uint64_t x = BetterSongSearch::dataHolder.songDetails->tags.at(t);
        res |= x;
    }

    return res;
}

void BetterSongSearch::FilterProfile::RecalculatePreprocessedValues() {
    if (charFilter == (int) FilterTypes::CharFilter::All) {
        charFilterPreprocessed = SongDetailsCache::MapCharacteristic::Custom;
    } else {
        charFilterPreprocessed = CHARACTERISTIC_MAP.at((FilterTypes::CharFilter) charFilter);
    }

    if (difficultyFilter == (int) FilterTypes::DifficultyFilter::All) {
        difficultyFilterPreprocessed = SongDetailsCache::MapDifficulty::Easy;
    } else {
        difficultyFilterPreprocessed = DIFFICULTY_MAP.at((FilterTypes::DifficultyFilter) difficultyFilter);
    }

    isDefaultPreprocessed = IsDefault();

    // Calculate bit fields
    _mapStyleBitfield = CalculateTagsBitfield(mapStyleString);
    _mapGenreBitfield = CalculateTagsBitfield(mapGenreString);
    _mapGenreExcludeBitfield = CalculateTagsBitfield(mapGenreExcludeString);
}

std::tuple<int, int> BetterSongSearch::FilterProfile::CountTags() {
    int included = 0;
    int excluded = 0;

    if (mapGenreString != "") {
        included += split(mapGenreString, " ").size();
    }

    if (mapGenreExcludeString != "") {
        excluded += split(mapGenreExcludeString, " ").size();
    }

    return {included, excluded};
}

bool BetterSongSearch::FilterProfile::DeletePreset(std::string presetName) {
    std::string presetsDir = getDataDir(modInfo) + "/Presets/";

    // Ensure the directory exists
    if (!direxists(presetsDir)) {
        mkpath(presetsDir);
    }

    std::string path = presetsDir + presetName + ".json";

    if (!fileexists(path)) {
        return false;
    }

    try {
        std::filesystem::remove(path);
        return true;
    } catch (std::exception const& e) {
        ERROR("Failed to delete preset: {}", e.what());
        return false;
    }
}

bool BetterSongSearch::FilterProfile::IsEqual(BetterSongSearch::FilterProfile const& other) const {
    // clang-format off
    return (
        downloadType == other.downloadType &&
        localScoreType == other.localScoreType &&
        minLength == other.minLength &&
        maxLength == other.maxLength &&
        minNJS == other.minNJS &&
        maxNJS == other.maxNJS &&
        minNPS == other.minNPS &&
        maxNPS == other.maxNPS &&
        rankedType == other.rankedType &&
        minStars == other.minStars &&
        maxStars == other.maxStars &&
        minUploadDate == other.minUploadDate &&
        minRating == other.minRating &&
        minVotes == other.minVotes &&
        charFilter == other.charFilter &&
        difficultyFilter == other.difficultyFilter &&
        modRequirement == other.modRequirement &&
        minUploadDateInMonths == other.minUploadDateInMonths &&
        onlyCuratedMaps == other.onlyCuratedMaps &&
        onlyVerifiedMappers == other.onlyVerifiedMappers &&
        onlyV3Maps == other.onlyV3Maps &&
        uploaders == other.uploaders &&
        uploadersBlackList == other.uploadersBlackList &&
        mapStyleString == other.mapStyleString &&
        mapGenreString == other.mapGenreString &&
        mapGenreExcludeString == other.mapGenreExcludeString
    );
    // clang-format on
}

void BetterSongSearch::FilterProfile::PrintToDebug() {
    DEBUG("FilterProfile: ");
    DEBUG("DownloadType: {}", downloadType);
    DEBUG("LocalScoreType: {}", localScoreType);
    DEBUG("RankedType: {}", rankedType);
    DEBUG("DifficultyFilter: {}", difficultyFilter);
    DEBUG("ModRequirement: {}", modRequirement);
    DEBUG("MinLength: {}", minLength);
    DEBUG("MaxLength: {}", maxLength);
    DEBUG("MinNJS: {}", minNJS);
    DEBUG("MaxNJS: {}", maxNJS);
    DEBUG("MinNPS: {}", minNPS);
    DEBUG("MaxNPS: {}", maxNPS);
    DEBUG("MinStars: {}", minStars);
    DEBUG("MaxStars: {}", maxStars);
    DEBUG("MinUploadDate: {}", minUploadDate);
    DEBUG("MinRating: {}", minRating);
    DEBUG("MinVotes: {}", minVotes);
    DEBUG("CharFilter: {}", charFilter);
    DEBUG("MinUploadDateInMonths: {}", minUploadDateInMonths);
    DEBUG("Uploaders: {}", join(uploaders, " "));
    DEBUG("UploadersBlackList: {}", uploadersBlackList);
    DEBUG("OnlyCuratedMaps: {}", onlyCuratedMaps);
    DEBUG("OnlyVerifiedMappers: {}", onlyVerifiedMappers);
    DEBUG("OnlyV3Maps: {}", onlyV3Maps);
    DEBUG("MapStyleString: {}", mapStyleString);
    DEBUG("MapGenreString: {}", mapGenreString);
    DEBUG("MapGenreExcludeString: {}", mapGenreExcludeString);
}
