#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include "GlobalNamespace/LevelCollectionViewController.hpp"
#include "GlobalNamespace/SongPreviewPlayer.hpp"
#include "HMUI/CurvedCanvasSettings.hpp"
#include "HMUI/CurvedTextMeshPro.hpp"
#include "TMPro/TextMeshProUGUI.hpp"
#include "UnityEngine/RectTransform.hpp"
#include "UnityEngine/Resources.hpp"
#include "UnityEngine/Sprite.hpp"
#include "UnityEngine/UI/LayoutRebuilder.hpp"
#include "beatsaverplusplus/shared/BeatSaver.hpp"
#include "bsml/shared/BSML.hpp"
#include "bsml/shared/BSML/Components/Backgroundable.hpp"
#include "bsml/shared/BSML/Components/HotReloadFileWatcher.hpp"
#include "bsml/shared/Helpers/getters.hpp"
#include "song-details/shared/SongDetails.hpp"
#include "songcore/shared/SongCore.hpp"
#include "web-utils/shared/WebUtils.hpp"
#include <bsml/shared/BSML/Animations/AnimationStateUpdater.hpp>
#include <bsml/shared/BSML/Components/ButtonIconImage.hpp>

#include "assets.hpp"
#include "CustomSongFilter.hpp"
#include "HMUI/Touchable.hpp"
#include "Log.hpp"
#include "Configuration.hpp"
#include "JapaneseConverter.hpp"
#include "SpriteCache.hpp"
#include "UI/FlowCoordinators/AirbudsSearchFlowCoordinator.hpp"
#include "UI/TableViewDataSources/CustomSongTableViewDataSource.hpp"
#include "UI/TableViewDataSources/DownloadHistoryTableViewDataSource.hpp"
#include "UI/TableViewDataSources/AirbudsPlaylistTableViewDataSource.hpp"
#include "UI/TableViewDataSources/AirbudsTrackTableViewDataSource.hpp"
#include "UI/ViewControllers/MainViewController.hpp"
#include "Utils.hpp"
#include "main.hpp"
#include "scotland2/shared/modloader.h"

#include <dlfcn.h>
#include <cstring>

DEFINE_TYPE(AirbudsSearch::UI::ViewControllers, MainViewController);

using namespace AirbudsSearch::UI::ViewControllers;

namespace AirbudsSearch::Filter {

static bool isHiragana(uint32_t codepoint);
static bool isKatakana(uint32_t codepoint);
static bool isKanji(uint32_t codepoint);
static std::vector<uint32_t> decodeUtf8(const std::string& text, bool& hasKana, bool& hasKanji);

static std::vector<std::string> getWords(const std::string& text) {
    std::vector<std::string> words;
    bool hasKana = false;
    bool hasKanji = false;
    const std::vector<uint32_t> codepoints = decodeUtf8(text, hasKana, hasKanji);
    std::string current;
    current.reserve(text.size());

    auto flush = [&]() {
        if (!current.empty()) {
            words.emplace_back(AirbudsSearch::Utils::toLowerCase(current));
            current.clear();
        }
    };

    auto appendUtf8 = [&](const uint32_t codepoint) {
        if (codepoint <= 0x7F) {
            current.push_back(static_cast<char>(codepoint));
            return;
        }
        if (codepoint <= 0x7FF) {
            current.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
            current.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
            return;
        }
        if (codepoint <= 0xFFFF) {
            current.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
            current.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
            current.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
            return;
        }
        current.push_back(static_cast<char>(0xF0 | (codepoint >> 18)));
        current.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
        current.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
        current.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    };

    auto isWordCodepoint = [&](const uint32_t codepoint) {
        if (codepoint <= 0x7F) {
            return std::isalnum(static_cast<unsigned char>(codepoint)) != 0;
        }
        return isHiragana(codepoint)
            || isKatakana(codepoint)
            || isKanji(codepoint)
            || codepoint == 0x30FC
            || codepoint == 0x3005;
    };

    for (const uint32_t codepoint : codepoints) {
        if (isWordCodepoint(codepoint)) {
            appendUtf8(codepoint);
            continue;
        }
        flush();
    }
    flush();

    return words;
}

static bool isHiragana(const uint32_t codepoint) {
    return codepoint >= 0x3040 && codepoint <= 0x309F;
}

static bool isKatakana(const uint32_t codepoint) {
    return codepoint >= 0x30A0 && codepoint <= 0x30FF;
}

static bool isKanji(const uint32_t codepoint) {
    return (codepoint >= 0x4E00 && codepoint <= 0x9FFF)
        || (codepoint >= 0x3400 && codepoint <= 0x4DBF)
        || (codepoint >= 0xF900 && codepoint <= 0xFAFF);
}

static bool isJapaneseCodepoint(const uint32_t codepoint) {
    return isHiragana(codepoint)
        || isKatakana(codepoint)
        || isKanji(codepoint)
        || codepoint == 0x3005;
}

static uint32_t toHiragana(const uint32_t codepoint) {
    if (codepoint >= 0x30A1 && codepoint <= 0x30F6) {
        return codepoint - 0x60;
    }
    return codepoint;
}

static bool isSmallVowel(const uint32_t codepoint) {
    switch (codepoint) {
        case 0x3041:
        case 0x3043:
        case 0x3045:
        case 0x3047:
        case 0x3049:
            return true;
        default:
            return false;
    }
}

static bool isSmallY(const uint32_t codepoint) {
    switch (codepoint) {
        case 0x3083:
        case 0x3085:
        case 0x3087:
            return true;
        default:
            return false;
    }
}

static const char* getSmallVowelRomaji(const uint32_t codepoint) {
    switch (codepoint) {
        case 0x3041:
            return "a";
        case 0x3043:
            return "i";
        case 0x3045:
            return "u";
        case 0x3047:
            return "e";
        case 0x3049:
            return "o";
        default:
            return "";
    }
}

static const char* getSmallYRomaji(const uint32_t codepoint) {
    switch (codepoint) {
        case 0x3083:
            return "ya";
        case 0x3085:
            return "yu";
        case 0x3087:
            return "yo";
        default:
            return "";
    }
}

static const char* kanaToRomaji(const uint32_t codepoint) {
    switch (codepoint) {
        case 0x3042: return "a";
        case 0x3044: return "i";
        case 0x3046: return "u";
        case 0x3048: return "e";
        case 0x304A: return "o";
        case 0x304B: return "ka";
        case 0x304D: return "ki";
        case 0x304F: return "ku";
        case 0x3051: return "ke";
        case 0x3053: return "ko";
        case 0x3055: return "sa";
        case 0x3057: return "shi";
        case 0x3059: return "su";
        case 0x305B: return "se";
        case 0x305D: return "so";
        case 0x305F: return "ta";
        case 0x3061: return "chi";
        case 0x3064: return "tsu";
        case 0x3066: return "te";
        case 0x3068: return "to";
        case 0x306A: return "na";
        case 0x306B: return "ni";
        case 0x306C: return "nu";
        case 0x306D: return "ne";
        case 0x306E: return "no";
        case 0x306F: return "ha";
        case 0x3072: return "hi";
        case 0x3075: return "fu";
        case 0x3078: return "he";
        case 0x307B: return "ho";
        case 0x307E: return "ma";
        case 0x307F: return "mi";
        case 0x3080: return "mu";
        case 0x3081: return "me";
        case 0x3082: return "mo";
        case 0x3084: return "ya";
        case 0x3086: return "yu";
        case 0x3088: return "yo";
        case 0x3089: return "ra";
        case 0x308A: return "ri";
        case 0x308B: return "ru";
        case 0x308C: return "re";
        case 0x308D: return "ro";
        case 0x308F: return "wa";
        case 0x3090: return "wi";
        case 0x3091: return "we";
        case 0x3092: return "o";
        case 0x3093: return "n";
        case 0x304C: return "ga";
        case 0x304E: return "gi";
        case 0x3050: return "gu";
        case 0x3052: return "ge";
        case 0x3054: return "go";
        case 0x3056: return "za";
        case 0x3058: return "ji";
        case 0x305A: return "zu";
        case 0x305C: return "ze";
        case 0x305E: return "zo";
        case 0x3060: return "da";
        case 0x3062: return "ji";
        case 0x3065: return "zu";
        case 0x3067: return "de";
        case 0x3069: return "do";
        case 0x3070: return "ba";
        case 0x3073: return "bi";
        case 0x3076: return "bu";
        case 0x3079: return "be";
        case 0x307C: return "bo";
        case 0x3071: return "pa";
        case 0x3074: return "pi";
        case 0x3077: return "pu";
        case 0x307A: return "pe";
        case 0x307D: return "po";
        case 0x3094: return "vu";
        case 0x3095: return "ka";
        case 0x3096: return "ke";
        default:
            return "";
    }
}

static char getLastVowel(const std::string& text) {
    for (auto it = text.rbegin(); it != text.rend(); ++it) {
        switch (*it) {
            case 'a':
            case 'i':
            case 'u':
            case 'e':
            case 'o':
                return *it;
            default:
                break;
        }
    }
    return '\0';
}

static void appendSpaceIfNeeded(std::string& text) {
    if (!text.empty() && text.back() != ' ') {
        text.push_back(' ');
    }
}

static void appendUtf8(std::string& text, const uint32_t codepoint) {
    if (codepoint <= 0x7F) {
        text.push_back(static_cast<char>(codepoint));
        return;
    }
    if (codepoint <= 0x7FF) {
        text.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
        text.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
        return;
    }
    if (codepoint <= 0xFFFF) {
        text.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
        text.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
        text.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
        return;
    }
    text.push_back(static_cast<char>(0xF0 | (codepoint >> 18)));
    text.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
    text.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
    text.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
}

static std::string doubleLeadingConsonant(const std::string& romaji) {
    if (romaji.empty()) {
        return romaji;
    }
    const char first = romaji.front();
    if (first == 'a' || first == 'i' || first == 'u' || first == 'e' || first == 'o') {
        return romaji;
    }
    return std::string(1, first) + romaji;
}

static std::string combineYoon(const std::string& base, const uint32_t smallY) {
    const char* y = getSmallYRomaji(smallY);
    if (!y || y[0] == '\0') {
        return "";
    }

    if (base == "shi") {
        return std::string("sh") + y;
    }
    if (base == "chi") {
        return std::string("ch") + y;
    }
    if (base == "ji") {
        return std::string("j") + y;
    }
    if (!base.empty() && base.back() == 'i') {
        return base.substr(0, base.size() - 1) + y;
    }
    return "";
}

static std::string combineSmallVowel(const std::string& base, const uint32_t smallVowel) {
    const char* vowel = getSmallVowelRomaji(smallVowel);
    if (!vowel || vowel[0] == '\0') {
        return "";
    }

    const char v = vowel[0];
    if (base == "fu") {
        return std::string("f") + vowel;
    }
    if (base == "vu") {
        return std::string("v") + vowel;
    }
    if (base == "te" && v == 'i') {
        return "ti";
    }
    if (base == "de" && v == 'i') {
        return "di";
    }
    if (base == "to" && v == 'u') {
        return "tu";
    }
    if (base == "do" && v == 'u') {
        return "du";
    }
    if (base == "shi" && v == 'e') {
        return "she";
    }
    if (base == "chi" && v == 'e') {
        return "che";
    }
    if (base == "ji" && v == 'e') {
        return "je";
    }
    if (base == "su" && v == 'i') {
        return "si";
    }
    if (base == "zu" && v == 'i') {
        return "zi";
    }
    if (base == "tsu") {
        switch (v) {
            case 'a': return "tsa";
            case 'i': return "tsi";
            case 'e': return "tse";
            case 'o': return "tso";
            default: break;
        }
    }
    if (base == "ku") {
        switch (v) {
            case 'a': return "kwa";
            case 'i': return "kwi";
            case 'e': return "kwe";
            case 'o': return "kwo";
            default: break;
        }
    }
    if (base == "gu") {
        switch (v) {
            case 'a': return "gwa";
            case 'i': return "gwi";
            case 'e': return "gwe";
            case 'o': return "gwo";
            default: break;
        }
    }
    return "";
}

static std::vector<uint32_t> decodeUtf8(const std::string& text, bool& hasKana, bool& hasKanji) {
    std::vector<uint32_t> codepoints;
    hasKana = false;
    hasKanji = false;
    for (size_t i = 0; i < text.size();) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (c < 0x80) {
            codepoints.push_back(c);
            ++i;
            continue;
        }

        uint32_t codepoint = 0;
        size_t extraBytes = 0;
        if ((c & 0xE0) == 0xC0 && i + 1 < text.size()) {
            codepoint = ((c & 0x1F) << 6) | (static_cast<unsigned char>(text[i + 1]) & 0x3F);
            extraBytes = 1;
        } else if ((c & 0xF0) == 0xE0 && i + 2 < text.size()) {
            codepoint = ((c & 0x0F) << 12)
                | ((static_cast<unsigned char>(text[i + 1]) & 0x3F) << 6)
                | (static_cast<unsigned char>(text[i + 2]) & 0x3F);
            extraBytes = 2;
        } else if ((c & 0xF8) == 0xF0 && i + 3 < text.size()) {
            codepoint = ((c & 0x07) << 18)
                | ((static_cast<unsigned char>(text[i + 1]) & 0x3F) << 12)
                | ((static_cast<unsigned char>(text[i + 2]) & 0x3F) << 6)
                | (static_cast<unsigned char>(text[i + 3]) & 0x3F);
            extraBytes = 3;
        } else {
            ++i;
            continue;
        }

        codepoints.push_back(codepoint);
        if (isHiragana(codepoint) || isKatakana(codepoint) || codepoint == 0x30FC) {
            hasKana = true;
        } else if (isKanji(codepoint)) {
            hasKanji = true;
        }
        i += extraBytes + 1;
    }

    return codepoints;
}

static std::thread::id mainThreadId;

static void captureMainThreadId() {
    if (mainThreadId == std::thread::id()) {
        mainThreadId = std::this_thread::get_id();
    }
}

static bool isOnMainThread() {
    return mainThreadId != std::thread::id() && std::this_thread::get_id() == mainThreadId;
}

static std::once_flag romajiOverridesInitFlag;
static std::vector<std::pair<std::string, std::string>> romajiOverrides;

static std::string trimAscii(const std::string& text) {
    size_t start = 0;
    while (start < text.size() && std::isspace(static_cast<unsigned char>(text[start]))) {
        ++start;
    }
    if (start >= text.size()) {
        return "";
    }
    size_t end = text.size() - 1;
    while (end > start && std::isspace(static_cast<unsigned char>(text[end]))) {
        --end;
    }
    return text.substr(start, end - start + 1);
}

static void loadRomajiOverrides() {
    std::call_once(romajiOverridesInitFlag, []() {
        const std::filesystem::path overridePath = AirbudsSearch::getDataDirectory() / "romaji_overrides.txt";
        if (!std::filesystem::exists(overridePath)) {
            return;
        }
        std::ifstream file(overridePath, std::ios::binary);
        if (!file.is_open()) {
            AirbudsSearch::Log.warn("Romaji overrides file could not be opened: {}", overridePath.string());
            return;
        }

        std::string line;
        while (std::getline(file, line)) {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            const std::string trimmed = trimAscii(line);
            if (trimmed.empty() || trimmed.front() == '#') {
                continue;
            }
            size_t delimiter = trimmed.find('=');
            if (delimiter == std::string::npos) {
                delimiter = trimmed.find('\t');
            }
            if (delimiter == std::string::npos) {
                continue;
            }
            std::string source = trimAscii(trimmed.substr(0, delimiter));
            std::string romaji = trimAscii(trimmed.substr(delimiter + 1));
            if (source.empty() || romaji.empty()) {
                continue;
            }
            romajiOverrides.emplace_back(std::move(source), std::move(romaji));
        }

        if (!romajiOverrides.empty()) {
            std::sort(romajiOverrides.begin(), romajiOverrides.end(),
                [](const auto& left, const auto& right) {
                    return left.first.size() > right.first.size();
                });
            AirbudsSearch::Log.info("Loaded {} romaji overrides from {}", romajiOverrides.size(), overridePath.string());
        } else {
            AirbudsSearch::Log.warn("Romaji overrides file is empty: {}", overridePath.string());
        }
    });
}

static std::string applyRomajiOverrides(const std::string& text) {
    loadRomajiOverrides();
    if (romajiOverrides.empty() || text.empty()) {
        return text;
    }

    std::string output = text;
    for (const auto& entry : romajiOverrides) {
        const std::string& needle = entry.first;
        const std::string& replacement = entry.second;
        if (needle.empty()) {
            continue;
        }
        size_t pos = 0;
        while ((pos = output.find(needle, pos)) != std::string::npos) {
            output.replace(pos, needle.size(), replacement);
            pos += replacement.size();
        }
    }
    return output;
}

static std::once_flag converterInitFlag;
static const AirbudsSearch::IJapaneseConverter* externalConverter = nullptr;
static std::string externalConverterName;
static std::once_flag converterMissingLogFlag;
static std::once_flag converterInvalidLogFlag;
static std::once_flag converterFailureLogFlag;

static const AirbudsSearch::IJapaneseConverter* loadExternalJapaneseConverter() {
    std::call_once(converterInitFlag, []() {
        CModInfo modInfo{"airbuds-search-kakasi", "0.0.0", 0};
        CModResult mod = modloader_get_mod(&modInfo, MatchType_IdOnly);
        void* symbol = nullptr;
        if (mod.handle) {
            symbol = dlsym(mod.handle, AirbudsSearch::kJapaneseConverterSymbol);
        }
        if (!symbol) {
            symbol = dlsym(RTLD_DEFAULT, AirbudsSearch::kJapaneseConverterSymbol);
        }
        if (!symbol) {
            std::call_once(converterMissingLogFlag, []() {
                AirbudsSearch::Log.info("Japanese converter mod not found; romaji will be kana-only.");
            });
            return;
        }

        auto getConverter = reinterpret_cast<AirbudsSearch::GetJapaneseConverterFn>(symbol);
        const AirbudsSearch::IJapaneseConverter* converter = getConverter ? getConverter() : nullptr;
        if (!converter || converter->apiVersion != 1 || !converter->convert) {
            std::call_once(converterInvalidLogFlag, []() {
                AirbudsSearch::Log.warn("Japanese converter API mismatch; romaji will be kana-only.");
            });
            return;
        }

        externalConverter = converter;
        externalConverterName = converter->name ? converter->name : "unknown";
        AirbudsSearch::Log.info("Japanese converter loaded: {}", externalConverterName);
    });
    return externalConverter;
}

static std::string romanizeWithExternalConverter(const std::string& text) {
    const AirbudsSearch::IJapaneseConverter* converter = loadExternalJapaneseConverter();
    if (!converter || text.empty()) {
        return "";
    }

    std::string output;
    output.resize(text.size() * 4 + 32);
    if (!converter->convert(text.c_str(), output.data(), output.size())) {
        std::call_once(converterFailureLogFlag, []() {
            AirbudsSearch::Log.warn("Japanese converter failed to convert text.");
        });
        return "";
    }

    output.resize(std::strlen(output.c_str()));
    return output;
}

static std::string normalizeRomajiAscii(const std::string& text) {
    std::string normalized;
    normalized.reserve(text.size());
    for (unsigned char c : text) {
        if (std::isalnum(c)) {
            normalized.push_back(static_cast<char>(std::tolower(c)));
        }
    }
    return normalized;
}

static std::string romanizeKanaOnly(const std::vector<uint32_t>& codepoints) {
    std::string output;
    bool doubleConsonant = false;

    for (size_t i = 0; i < codepoints.size(); ++i) {
        uint32_t codepoint = codepoints[i];

        if (codepoint <= 0x7F) {
            output.push_back(static_cast<char>(codepoint));
            continue;
        }

        if (codepoint == 0x3000 || codepoint == 0x30FB) {
            appendSpaceIfNeeded(output);
            continue;
        }

        if (codepoint == 0x30FC) {
            const char vowel = getLastVowel(output);
            if (vowel != '\0') {
                output.push_back(vowel);
            }
            continue;
        }

        if (isKatakana(codepoint)) {
            codepoint = toHiragana(codepoint);
        }

        if (isHiragana(codepoint)) {
            if (codepoint == 0x3063) {
                doubleConsonant = true;
                continue;
            }

            uint32_t nextCodepoint = 0;
            if (i + 1 < codepoints.size()) {
                nextCodepoint = codepoints[i + 1];
                if (isKatakana(nextCodepoint)) {
                    nextCodepoint = toHiragana(nextCodepoint);
                }
            }

            const std::string base = kanaToRomaji(codepoint);
            if (!base.empty()) {
                if (isSmallY(nextCodepoint)) {
                    std::string combined = combineYoon(base, nextCodepoint);
                    if (!combined.empty()) {
                        if (doubleConsonant) {
                            combined = doubleLeadingConsonant(combined);
                        }
                        output += combined;
                        doubleConsonant = false;
                        ++i;
                        continue;
                    }
                }

                if (isSmallVowel(nextCodepoint)) {
                    std::string combined = combineSmallVowel(base, nextCodepoint);
                    if (!combined.empty()) {
                        if (doubleConsonant) {
                            combined = doubleLeadingConsonant(combined);
                        }
                        output += combined;
                        doubleConsonant = false;
                        ++i;
                        continue;
                    }
                }

                std::string romaji = base;
                if (doubleConsonant) {
                    romaji = doubleLeadingConsonant(romaji);
                }
                output += romaji;
                doubleConsonant = false;
                continue;
            }

            if (isSmallVowel(codepoint)) {
                const char* vowel = getSmallVowelRomaji(codepoint);
                if (vowel[0] != '\0') {
                    output += vowel;
                }
                continue;
            }

            if (isSmallY(codepoint)) {
                const char* y = getSmallYRomaji(codepoint);
                if (y[0] != '\0') {
                    output += y;
                }
                continue;
            }

            appendSpaceIfNeeded(output);
            doubleConsonant = false;
            continue;
        }

        appendSpaceIfNeeded(output);
    }

    return output;
}

static std::string romanizeJapaneseSegment(
    const std::string& text,
    const std::vector<uint32_t>& codepoints,
    const bool hasKana,
    const bool hasKanji) {
    if (hasKanji) {
        const std::string romaji = romanizeWithExternalConverter(text);
        if (!romaji.empty()) {
            if (hasKana) {
                const std::string kanaOnly = romanizeKanaOnly(codepoints);
                if (!kanaOnly.empty()
                    && normalizeRomajiAscii(romaji) == normalizeRomajiAscii(kanaOnly)) {
                    return "";
                }
            }
            return romaji;
        }
        if (hasKana) {
            return romanizeKanaOnly(codepoints);
        }
        return "";
    }
    if (!hasKana) {
        return "";
    }
    return romanizeKanaOnly(codepoints);
}

static std::string romanizeJapanese(const std::string& text) {
    const std::string input = applyRomajiOverrides(text);
    const bool overridesApplied = input != text;
    bool hasKana = false;
    bool hasKanji = false;
    const std::vector<uint32_t> codepoints = decodeUtf8(input, hasKana, hasKanji);
    if (!hasKana && !hasKanji) {
        if (!overridesApplied) {
            return "";
        }
        std::string output;
        output.reserve(input.size());
        for (unsigned char c : input) {
            if (std::isalnum(c)) {
                output.push_back(static_cast<char>(std::tolower(c)));
            } else {
                appendSpaceIfNeeded(output);
            }
        }
        if (!output.empty() && output.back() == ' ') {
            output.pop_back();
        }
        return output;
    }

    std::string output;
    std::string jpSegment;
    std::vector<uint32_t> jpCodepoints;
    bool segmentHasKana = false;
    bool segmentHasKanji = false;

    auto flushSegment = [&]() {
        if (jpSegment.empty()) {
            return;
        }
        const std::string romaji = romanizeJapaneseSegment(jpSegment, jpCodepoints, segmentHasKana, segmentHasKanji);
        if (!romaji.empty()) {
            appendSpaceIfNeeded(output);
            output += romaji;
        }
        jpSegment.clear();
        jpCodepoints.clear();
        segmentHasKana = false;
        segmentHasKanji = false;
    };

    for (const uint32_t codepoint : codepoints) {
        if (isJapaneseCodepoint(codepoint)) {
            appendUtf8(jpSegment, codepoint);
            jpCodepoints.push_back(codepoint);
            if (isKanji(codepoint)) {
                segmentHasKanji = true;
            } else if (isHiragana(codepoint) || isKatakana(codepoint)) {
                segmentHasKana = true;
            }
            continue;
        }

        flushSegment();

        if (codepoint <= 0x7F) {
            const unsigned char ascii = static_cast<unsigned char>(codepoint);
            if (std::isalnum(ascii)) {
                output.push_back(static_cast<char>(std::tolower(ascii)));
            } else {
                appendSpaceIfNeeded(output);
            }
            continue;
        }

        appendSpaceIfNeeded(output);
    }

    flushSegment();
    if (!output.empty() && output.back() == ' ') {
        output.pop_back();
    }

    return output;
}

static std::vector<std::string> getWordsWithRomaji(const std::string& text, const std::string& romaji) {
    std::vector<std::string> words = getWords(text);
    if (!romaji.empty()) {
        const std::vector<std::string> romajiWords = getWords(romaji);
        words.insert(words.end(), romajiWords.begin(), romajiWords.end());
    }
    return words;
}

static std::vector<std::string> getWordsWithRomaji(const std::string& text) {
    return getWordsWithRomaji(text, romanizeJapanese(text));
}

static std::string getTrackRomajiCached(const airbuds::Track& track) {
    if (track.name.empty()) {
        return "";
    }

    const std::string key = track.id.empty() ? track.name : track.id;
    static std::mutex cacheMutex;
    static std::unordered_map<std::string, std::string> cache;

    {
        std::lock_guard<std::mutex> lock(cacheMutex);
        auto it = cache.find(key);
        if (it != cache.end()) {
            return it->second;
        }
    }

    std::string romaji = romanizeJapanese(track.name);
    {
        std::lock_guard<std::mutex> lock(cacheMutex);
        cache.emplace(key, romaji);
    }

    return romaji;
}

struct QuerySpec {
    std::string query;
    std::string label;
    int baseScore = 0;
};

struct ArtistMatchInfo {
    std::string name;
    std::string romaji;
};

static std::string normalizeQueryWhitespace(const std::string& text) {
    std::string output;
    output.reserve(text.size());
    bool inWhitespace = false;
    for (unsigned char c : text) {
        if (std::isspace(c)) {
            if (!output.empty() && !inWhitespace) {
                output.push_back(' ');
                inWhitespace = true;
            }
            continue;
        }
        output.push_back(static_cast<char>(c));
        inWhitespace = false;
    }
    if (!output.empty() && output.back() == ' ') {
        output.pop_back();
    }
    return output;
}

static std::string joinWords(const std::vector<std::string>& words) {
    std::string output;
    for (const std::string& word : words) {
        if (word.empty()) {
            continue;
        }
        if (!output.empty()) {
            output.push_back(' ');
        }
        output.append(word);
    }
    return normalizeQueryWhitespace(output);
}

static std::string buildQueryFromText(const std::string& text) {
    const std::vector<std::string> words = getWords(text);
    if (words.empty()) {
        return normalizeQueryWhitespace(text);
    }
    return joinWords(words);
}

static size_t countAsciiAlnum(const std::string& text) {
    size_t count = 0;
    for (unsigned char c : text) {
        if (std::isalnum(c)) {
            ++count;
        }
    }
    return count;
}

static bool isUsefulRomajiQuery(const std::string& romajiQuery) {
    if (romajiQuery.empty()) {
        return false;
    }
    if (romajiQuery.find(' ') != std::string::npos) {
        return true;
    }
    return countAsciiAlnum(romajiQuery) >= 4;
}

static void addUniqueQuery(std::vector<std::string>& queries, std::unordered_set<std::string>& seen, const std::string& rawQuery) {
    const std::string normalized = normalizeQueryWhitespace(rawQuery);
    if (normalized.empty()) {
        return;
    }
    std::string key = normalizeRomajiAscii(normalized);
    if (key.empty()) {
        key = normalized;
    }
    if (seen.insert(key).second) {
        queries.push_back(normalized);
    }
}

static std::string urlEncodeQuery(std::string_view text) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string output;
    output.reserve(text.size() * 3);
    for (unsigned char c : text) {
        if ((c >= 'A' && c <= 'Z')
            || (c >= 'a' && c <= 'z')
            || (c >= '0' && c <= '9')
            || c == '-' || c == '_' || c == '.' || c == '~') {
            output.push_back(static_cast<char>(c));
            continue;
        }
        output.push_back('%');
        output.push_back(kHex[(c >> 4) & 0xF]);
        output.push_back(kHex[c & 0xF]);
    }
    return output;
}

static std::vector<ArtistMatchInfo> buildArtistInfos(const std::vector<airbuds::Artist>& artists) {
    std::vector<ArtistMatchInfo> infos;
    infos.reserve(artists.size());
    for (const airbuds::Artist& artist : artists) {
        ArtistMatchInfo info;
        info.name = artist.name;
        info.romaji = romanizeJapanese(artist.name);
        infos.push_back(std::move(info));
    }
    return infos;
}

static std::string getArtistRomajiLog(const std::vector<ArtistMatchInfo>& infos) {
    std::string output;
    for (const auto& info : infos) {
        if (info.romaji.empty()) {
            continue;
        }
        if (!output.empty()) {
            output += ", ";
        }
        output += info.romaji;
    }
    return output;
}

static void addQuerySpec(
    std::vector<QuerySpec>& queries,
    std::unordered_map<std::string, size_t>& seen,
    const std::string& rawQuery,
    std::string_view label,
    int baseScore) {
    const std::string normalized = normalizeQueryWhitespace(rawQuery);
    if (normalized.empty()) {
        return;
    }
    std::string key = normalizeRomajiAscii(normalized);
    if (key.empty()) {
        key = normalized;
    }
    auto it = seen.find(key);
    if (it != seen.end()) {
        QuerySpec& existing = queries[it->second];
        if (baseScore > existing.baseScore) {
            existing.baseScore = baseScore;
            existing.label = std::string(label);
        }
        return;
    }
    QuerySpec spec;
    spec.query = normalized;
    spec.label = std::string(label);
    spec.baseScore = baseScore;
    seen.emplace(key, queries.size());
    queries.push_back(std::move(spec));
}

static std::vector<QuerySpec> buildBeatSaverQueries(const airbuds::Track& track, const std::vector<ArtistMatchInfo>& artistInfos) {
    std::vector<QuerySpec> queries;
    std::unordered_map<std::string, size_t> seen;

    const std::string nameQuery = buildQueryFromText(track.name);
    addQuerySpec(queries, seen, nameQuery, "name", 1500);

    std::string artistQuery;
    std::string artistRomajiQuery;
    if (!artistInfos.empty()) {
        artistQuery = buildQueryFromText(artistInfos.front().name);
        artistRomajiQuery = buildQueryFromText(artistInfos.front().romaji);
    }
    if (!nameQuery.empty() && !artistQuery.empty()) {
        addQuerySpec(queries, seen, std::format("{} {}", nameQuery, artistQuery), "name+artist", 2000);
    }
    if (!nameQuery.empty()
        && isUsefulRomajiQuery(artistRomajiQuery)
        && normalizeRomajiAscii(artistRomajiQuery) != normalizeRomajiAscii(artistQuery)) {
        addQuerySpec(queries, seen, std::format("{} {}", nameQuery, artistRomajiQuery), "name+artistRomaji", 1800);
    }

    const std::string romaji = getTrackRomajiCached(track);
    const std::string romajiQuery = buildQueryFromText(romaji);
    if (isUsefulRomajiQuery(romajiQuery)
        && normalizeRomajiAscii(romajiQuery) != normalizeRomajiAscii(nameQuery)) {
        addQuerySpec(queries, seen, romajiQuery, "romaji", 1200);
        if (!artistQuery.empty()) {
            addQuerySpec(queries, seen, std::format("{} {}", romajiQuery, artistQuery), "romaji+artist", 1400);
        }
        if (!artistRomajiQuery.empty()) {
            addQuerySpec(queries, seen, std::format("{} {}", romajiQuery, artistRomajiQuery), "romaji+artistRomaji", 1300);
        }
    }

    std::stable_sort(queries.begin(), queries.end(), [](const QuerySpec& a, const QuerySpec& b) {
        return a.baseScore > b.baseScore;
    });

    return queries;
}

static int scoreTextMatch(const std::string& needle, const std::string& haystack) {
    if (needle.empty() || haystack.empty()) {
        return 0;
    }
    auto equalsIgnoreCase = [](unsigned char c1, unsigned char c2) {
        return std::tolower(c1) == std::tolower(c2);
    };
    if (std::ranges::equal(needle, haystack, equalsIgnoreCase)) {
        return 800;
    }

    const std::vector<std::string> needleWords = getWords(needle);
    const std::vector<std::string> haystackWords = getWords(haystack);
    if (needleWords.empty() || haystackWords.empty()) {
        return 0;
    }

    std::unordered_set<std::string> haystackSet(haystackWords.begin(), haystackWords.end());
    int score = 0;
    for (const std::string& word : needleWords) {
        if (word.empty()) {
            continue;
        }
        if (haystackSet.contains(word)) {
            score += 40 + static_cast<int>(std::min<size_t>(word.size(), 8)) * 5;
        }
    }
    return score;
}

static bool beatmapHasDifficulty(const BeatSaver::Models::BeatmapVersion& version, const std::vector<SongDetailsCache::MapDifficulty>& difficulties) {
    if (difficulties.empty()) {
        return false;
    }
    for (const BeatSaver::Models::BeatmapDifficulty& diff : version.Diffs) {
        const std::string diffName = AirbudsSearch::Utils::toLowerCase(diff.Difficulty);
        for (const SongDetailsCache::MapDifficulty desired : difficulties) {
            switch (desired) {
                case SongDetailsCache::MapDifficulty::Easy:
                    if (diffName == "easy") return true;
                    break;
                case SongDetailsCache::MapDifficulty::Normal:
                    if (diffName == "normal") return true;
                    break;
                case SongDetailsCache::MapDifficulty::Hard:
                    if (diffName == "hard") return true;
                    break;
                case SongDetailsCache::MapDifficulty::Expert:
                    if (diffName == "expert") return true;
                    break;
                case SongDetailsCache::MapDifficulty::ExpertPlus:
                    if (diffName == "expertplus" || diffName == "expert+") return true;
                    break;
                default:
                    break;
            }
        }
    }
    return false;
}

static int scoreBeatSaverCandidate(
    const airbuds::Track& track,
    const std::string& trackRomaji,
    const std::vector<ArtistMatchInfo>& artistInfos,
    const BeatSaver::Models::Beatmap& beatmap,
    bool artistBoost,
    bool difficultyBonus) {
    int score = 0;

    const auto& metadata = beatmap.Metadata;
    const std::string& songName = metadata.SongName;
    const std::string& songAuthor = metadata.SongAuthorName;
    const std::string& levelAuthor = metadata.LevelAuthorName;

    int nameScore = std::max(scoreTextMatch(track.name, songName), scoreTextMatch(track.name, beatmap.Name));
    if (!trackRomaji.empty()) {
        const std::string songNameRomaji = romanizeJapanese(songName);
        nameScore = std::max(nameScore, scoreTextMatch(trackRomaji, songName));
        if (!songNameRomaji.empty()) {
            nameScore = std::max(nameScore, scoreTextMatch(trackRomaji, songNameRomaji));
        }
    }
    score += nameScore * 2;

    int artistScore = 0;
    for (const auto& artist : artistInfos) {
        artistScore = std::max(artistScore, scoreTextMatch(artist.name, songAuthor));
        artistScore = std::max(artistScore, scoreTextMatch(artist.name, levelAuthor));
        if (!artist.romaji.empty()) {
            artistScore = std::max(artistScore, scoreTextMatch(artist.romaji, songAuthor));
            artistScore = std::max(artistScore, scoreTextMatch(artist.romaji, levelAuthor));
        }
    }
    score += artistScore;
    if (artistBoost && artistScore > 0) {
        score += 200;
    }

    if (difficultyBonus) {
        score += 25;
    }

    if (beatmap.Stats.Score > 0.0f) {
        score += std::clamp(static_cast<int>(beatmap.Stats.Score * 10.0f), 0, 50);
    }

    return score;
}

SongFilterFunction DEFAULT_SONG_FILTER_FUNCTION = [](const SongDetailsCache::Song* const song, const airbuds::Track& track) {
    // Remove songs that don't have at least one word from the Airbuds track in the name
    const std::string romajiTrackName = getTrackRomajiCached(track);
    const std::vector<std::string> wordsInTrackName = getWordsWithRomaji(track.name, romajiTrackName);
    const std::vector<std::string> wordsInSongName = getWords(song->songName());

    bool didMatchAtLeastOneWordFromSongName = false;
    for (const std::string& word : wordsInTrackName) {
        if (std::ranges::find(wordsInSongName, word) != wordsInSongName.end()) {
            didMatchAtLeastOneWordFromSongName = true;
        }
    }

    return didMatchAtLeastOneWordFromSongName;
};

SongScoreFunction DEFAULT_SONG_SCORE_FUNCTION = [](const airbuds::Track& track, const SongDetailsCache::Song& song) {
    int score = 0;

    // Song name
    const std::string romajiTrackName = getTrackRomajiCached(track);
    auto equalsIgnoreCase = [](unsigned char c1, unsigned char c2) {
        return std::tolower(c1) == std::tolower(c2);
    };

    if (std::ranges::equal(
            song.songName(), track.name,
            equalsIgnoreCase)
        || (!romajiTrackName.empty() && std::ranges::equal(song.songName(), romajiTrackName, equalsIgnoreCase))) {
        score += 1000;
    } else {
        const std::vector<std::string> wordsInTrackName = getWordsWithRomaji(track.name, romajiTrackName);
        const std::vector<std::string> wordsInSongName = getWords(song.songName());

        // One point for every word in the song name
        for (const std::string& word : wordsInTrackName) {
            if (std::ranges::find(wordsInSongName, word) != wordsInSongName.end()) {
                score += (100.0f * ((float) word.size() / (float) 10));
            }
        }
    }

    // Song artists
    const std::string songArtistLowercase = AirbudsSearch::Utils::toLowerCase(song.songAuthorName());
    std::vector<std::string> trackArtists;
    for (const airbuds::Artist& artist : track.artists) {
        const std::string artistNameLowercase = AirbudsSearch::Utils::toLowerCase(artist.name);
        if (songArtistLowercase.contains(artistNameLowercase)) {
            score += 100;
        }
    }

    // Increase score based on upvotes
    score += std::min(80, static_cast<int>((float) song.upvotes / (float) 10));

    // Decrease score based on downvotes
    score -= std::min(80, static_cast<int>((float) song.downvotes / 5));

    return score;
};

} // namespace AirbudsSearch::Filter

namespace {

bool getLocalDateKey(const std::chrono::milliseconds& millis, std::string& output) {
    if (millis.count() <= 0) {
        return false;
    }

    const std::time_t seconds = std::chrono::duration_cast<std::chrono::seconds>(millis).count();
    std::tm localTime{};
#if defined(_WIN32)
    if (localtime_s(&localTime, &seconds) != 0) {
        return false;
    }
#else
    if (!localtime_r(&seconds, &localTime)) {
        return false;
    }
#endif

    char buffer[16];
    std::snprintf(
        buffer,
        sizeof(buffer),
        "%04d-%02d-%02d",
        localTime.tm_year + 1900,
        localTime.tm_mon + 1,
        localTime.tm_mday);
    output = buffer;
    return true;
}

std::string makeTrackKey(const airbuds::PlaylistTrack& track) {
    const long long millis = track.dateAdded_.count();
    if (millis > 0) {
        return track.id + "|" + std::to_string(millis);
    }
    if (!track.dateAdded.empty()) {
        return track.id + "|" + track.dateAdded;
    }
    return track.id;
}

}

void MainViewController::DidActivate(const bool isFirstActivation, const bool addedToHierarchy, const bool screenSystemDisabling) {
    AirbudsSearch::Filter::captureMainThreadId();

    if (isFirstActivation) {
        BSML::parse_and_construct(IncludedAssets::MainViewController_bsml, this->get_transform(), this);

#if HOT_RELOAD
        fileWatcher->filePath = "/sdcard/MainViewController.bsml";
        fileWatcher->checkInterval = 1.0f;
#endif
    }

    AirbudsSearch::Log.info("main activate, set return = false");
    AirbudsSearch::returnToAirbudsSearch = false;

    if (!isFirstActivation && !selectedPlaylist_ && !isLoadingMoreAirbudsPlaylists_) {
        reloadAirbudsPlaylistListView();
    }
}

void MainViewController::onTrackLoadError(const std::string& message) {
    airbudsTrackListLoadingIndicatorContainer_->get_gameObject()->set_active(false);
    airbudsListViewErrorContainer_->get_gameObject()->set_active(true);
    airbudsTrackListErrorMessageTextView_->set_text(message);
    airbudsTrackListView_->get_gameObject()->set_active(false);
    randomTrackButton_->get_gameObject()->set_active(false);
    setRandomScopeVisible(false);
}

void MainViewController::resetListError() {
    airbudsTrackListLoadingIndicatorContainer_->get_gameObject()->set_active(true);
    airbudsListViewStatusContainer_->get_gameObject()->set_active(false);
    airbudsListViewErrorContainer_->get_gameObject()->set_active(false);
}

void MainViewController::setRandomScopeVisible(const bool visible) {
    if (randomScopeToggleContainer_) {
        randomScopeToggleContainer_->get_gameObject()->set_active(visible);
        if (visible) {
            updateRandomScopeButtonLabel();
            BSML::MainThreadScheduler::ScheduleNextFrame([this]() {
                forceLayoutRebuild();
            });
        }
        return;
    }
    if (randomScopeButton_) {
        randomScopeButton_->get_gameObject()->set_active(visible);
        if (visible) {
            updateRandomScopeButtonLabel();
            BSML::MainThreadScheduler::ScheduleNextFrame([this]() {
                forceLayoutRebuild();
            });
        }
    }
}

void MainViewController::updateRandomScopeButtonLabel() {
    if (!randomScopeButtonTextView_) {
        return;
    }
    if (randomAcrossAllDays_) {
        randomScopeButtonTextView_->set_text("[x] All history");
    } else {
        randomScopeButtonTextView_->set_text("[ ] All history");
    }
}

void MainViewController::updateHistoryTitle() {
    if (!airbudsColumnTitleTextView_) {
        return;
    }
    airbudsColumnTitleTextView_->set_text(getHistoryTitle());
}

std::optional<std::string> MainViewController::getSelectedFriendId() const {
    if (!selectedFriend_ || selectedFriend_->id.empty()) {
        return std::nullopt;
    }
    return selectedFriend_->id;
}

bool MainViewController::historyContextMatches(const std::optional<std::string>& friendId) const {
    if (!friendId && !selectedFriend_) {
        return true;
    }
    if (friendId && selectedFriend_ && selectedFriend_->id == *friendId) {
        return true;
    }
    return false;
}

std::vector<airbuds::PlaylistTrack> MainViewController::getRecentlyPlayedCachedOnlyForCurrentUser() {
    if (!AirbudsSearch::airbudsClient) {
        return {};
    }
    if (selectedFriend_) {
        return AirbudsSearch::airbudsClient->getRecentlyPlayedCachedOnlyForUser(selectedFriend_->id);
    }
    return AirbudsSearch::airbudsClient->getRecentlyPlayedCachedOnly();
}

std::string MainViewController::getHistoryTitle() const {
    if (selectedPlaylist_) {
        return selectedPlaylist_->name;
    }
    if (selectedFriend_) {
        std::string label = selectedFriend_->displayName;
        if (label.empty()) {
            label = selectedFriend_->identifier;
        }
        if (label.empty()) {
            label = "Friend";
        }
        return std::format("{} History", label);
    }
    return "Select History";
}

void MainViewController::forceLayoutRebuild() {
    auto* rootRect = get_transform()->GetComponent<UnityEngine::RectTransform*>();
    if (rootRect) {
        UnityEngine::UI::LayoutRebuilder::ForceRebuildLayoutImmediate(rootRect);
    }
    if (airbudsPlaylistListView_) {
        auto* playlistRect = airbudsPlaylistListView_->get_transform()->GetComponent<UnityEngine::RectTransform*>();
        if (playlistRect) {
            UnityEngine::UI::LayoutRebuilder::ForceRebuildLayoutImmediate(playlistRect);
        }
    }
    if (airbudsTrackListView_) {
        auto* trackRect = airbudsTrackListView_->get_transform()->GetComponent<UnityEngine::RectTransform*>();
        if (trackRect) {
            UnityEngine::UI::LayoutRebuilder::ForceRebuildLayoutImmediate(trackRect);
        }
    }
    if (randomScopeToggleContainer_) {
        auto* randomRect = randomScopeToggleContainer_->get_transform()->GetComponent<UnityEngine::RectTransform*>();
        if (randomRect) {
            UnityEngine::UI::LayoutRebuilder::ForceRebuildLayoutImmediate(randomRect);
        }
    }
}

void MainViewController::reloadAirbudsTrackListView() {
    auto* trackTableViewDataSource = gameObject->GetComponent<AirbudsTrackTableViewDataSource*>();
    resetListError();
    airbudsTrackListView_->get_gameObject()->set_active(true);
    randomTrackButton_->get_gameObject()->set_active(false);
    const std::string playlistId = selectedPlaylist_->id;
    const std::optional<std::string> friendId = getSelectedFriendId();
    isLoadingMoreAirbudsTracks_ = true;
    std::thread([this, trackTableViewDataSource, playlistId, friendId]() {
        // Make sure the Airbuds client is still valid
        if (!AirbudsSearch::airbudsClient) {
            isLoadingMoreAirbudsTracks_ = false;
            return;
        }

        // Load tracks
        std::vector<airbuds::PlaylistTrack> tracks;
        try {
            if (friendId) {
                tracks = airbudsClient->getPlaylistTracksForUser(*friendId, playlistId);
            } else {
                tracks = airbudsClient->getPlaylistTracks(playlistId);
            }
        } catch (const std::exception& exception) {
            const std::string message = exception.what();
            AirbudsSearch::Log.error("Failed loading tracks: {}", message);
            BSML::MainThreadScheduler::Schedule([this, message, trackTableViewDataSource, friendId]() {
                if (!historyContextMatches(friendId)) {
                    return;
                }
                if (trackTableViewDataSource && trackTableViewDataSource->trackCount() > 0) {
                    airbudsTrackListLoadingIndicatorContainer_->get_gameObject()->set_active(false);
                    airbudsListViewErrorContainer_->get_gameObject()->set_active(false);
                    airbudsListViewStatusContainer_->get_gameObject()->set_active(true);
                    airbudsTrackListStatusTextView_->set_text("Refresh failed; showing cached history.");
                    airbudsTrackListView_->get_gameObject()->set_active(true);
                    randomTrackButton_->get_gameObject()->set_active(true);
                    setRandomScopeVisible(true);
                } else {
                    onTrackLoadError(message);
                }
            });
            isLoadingMoreAirbudsTracks_ = false;
            return;
        }
        BSML::MainThreadScheduler::Schedule([this, trackTableViewDataSource, tracks, playlistId, friendId]() {
            // Check if we still have a playlist selected
            if (!selectedPlaylist_) {
                AirbudsSearch::Log.warn("Ignoring track list update because the selected playlist is null!");
                isLoadingMoreAirbudsTracks_ = false;
                return;
            }

            // Check if we still have the same playlist selected
            if (selectedPlaylist_->id != playlistId) {
                AirbudsSearch::Log.warn("Ignoring track list update because the selected playlist has changed! (requested = {} / current = {})", playlistId, selectedPlaylist_->id);
                isLoadingMoreAirbudsTracks_ = false;
                return;
            }

            if (!historyContextMatches(friendId)) {
                AirbudsSearch::Log.warn("Ignoring track list update because the history user changed.");
                isLoadingMoreAirbudsTracks_ = false;
                return;
            }

            trackTableViewDataSource->setTracks(tracks, AirbudsTrackTableViewDataSource::Grouping::ByHour);
            airbudsListViewStatusContainer_->get_gameObject()->set_active(false);
            if (tracks.empty()) {
                airbudsListViewStatusContainer_->get_gameObject()->set_active(true);
                airbudsTrackListStatusTextView_->set_text("No cached history");
            } else if (AirbudsSearch::airbudsClient) {
                const std::string warning = AirbudsSearch::airbudsClient->getLastRecentlyPlayedWarning();
                if (!warning.empty()) {
                    airbudsListViewStatusContainer_->get_gameObject()->set_active(true);
                    airbudsTrackListStatusTextView_->set_text(warning);
                }
            }
            Utils::reloadDataKeepingPosition(airbudsTrackListView_->tableView);

            isLoadingMoreAirbudsTracks_ = false;
            airbudsTrackListLoadingIndicatorContainer_->get_gameObject()->set_active(false);

            if (!tracks.empty()) {
                randomTrackButton_->get_gameObject()->set_active(true);
                setRandomScopeVisible(true);
            }

            int targetRow = -1;
            if (pendingRandomTrack_) {
                targetRow = trackTableViewDataSource->getRowIndexForTrack(*pendingRandomTrack_);
                pendingRandomTrack_.reset();
            }
            if (targetRow < 0 && !tracks.empty()) {
                targetRow = trackTableViewDataSource->getRowIndexForTrackIndex(0);
            }
            if (targetRow >= 0) {
                airbudsTrackListView_->tableView->SelectCellWithIdx(targetRow, true);
            }
            forceLayoutRebuild();
        });
    }).detach();
}

void MainViewController::PostParse() {

    Utils::setIconScale(playlistsMenuButton_, 1.5f);

    randomTrackButton_->get_gameObject()->set_active(false);
    if (randomScopeButton_) {
        randomScopeButtonTextView_ = randomScopeButton_->GetComponentInChildren<TMPro::TextMeshProUGUI*>();
        updateRandomScopeButtonLabel();
        setRandomScopeVisible(false);
    }

    if (isLoadingMoreAirbudsPlaylists_ || isLoadingMoreAirbudsTracks_) {
        airbudsTrackListLoadingIndicatorContainer_->get_gameObject()->set_active(true);
    } else {
        airbudsTrackListLoadingIndicatorContainer_->get_gameObject()->set_active(false);
    }

    // todo: if active error
    airbudsListViewErrorContainer_->get_gameObject()->set_active(false);
    airbudsListViewStatusContainer_->get_gameObject()->set_active(false);

    searchResultsListViewErrorContainer_->get_gameObject()->set_active(false);
    if (isSearchInProgress_) {
        searchResultsListLoadingIndicatorContainer_->get_gameObject()->set_active(true);
    } else {
        searchResultsListLoadingIndicatorContainer_->get_gameObject()->set_active(false);
    }

    // Set up the Airbuds playlist list
    auto* playlistTableViewDataSource = gameObject->GetComponent<AirbudsPlaylistTableViewDataSource*>();
    if (!playlistTableViewDataSource) {
        playlistTableViewDataSource = gameObject->AddComponent<AirbudsPlaylistTableViewDataSource*>();
        reloadAirbudsPlaylistListView();
    }
    airbudsPlaylistListView_->tableView->SetDataSource(reinterpret_cast<HMUI::TableView::IDataSource*>(playlistTableViewDataSource), true);

    // Set up the Airbuds track list
    auto* trackTableViewDataSource = gameObject->GetComponent<AirbudsTrackTableViewDataSource*>();
    if (!trackTableViewDataSource) {
        trackTableViewDataSource = gameObject->AddComponent<AirbudsTrackTableViewDataSource*>();
        if (selectedPlaylist_) {
            reloadAirbudsTrackListView();
        }
    }
    airbudsTrackListView_->tableView->SetDataSource(reinterpret_cast<HMUI::TableView::IDataSource*>(trackTableViewDataSource), true);

    // Set up the search results list
    auto* customSongTableViewDataSource = gameObject->GetComponent<CustomSongTableViewDataSource*>();
    if (!customSongTableViewDataSource) {
        customSongTableViewDataSource = gameObject->AddComponent<CustomSongTableViewDataSource*>();
    }
    searchResultsList_->tableView->SetDataSource(reinterpret_cast<HMUI::TableView::IDataSource*>(customSongTableViewDataSource), true);

    if (selectedPlaylist_) {
        // Set title
        airbudsColumnTitleTextView_->set_text(selectedPlaylist_->name);

        // Enable button
        playlistsMenuButton_->get_gameObject()->set_active(true);

        randomTrackButton_->get_gameObject()->set_active(true);
        setRandomScopeVisible(true);

        // Set up the Airbuds track list
        //airbudsTrackListView_->get_gameObject()->set_active(true);
        //airbudsPlaylistListView_->get_gameObject()->set_active(false);
    } else {
        // Set title
        updateHistoryTitle();

        // Disable back button
        playlistsMenuButton_->get_gameObject()->set_active(false);

        // Set up the Airbuds playlist list
        //airbudsTrackListView_->get_gameObject()->set_active(false);
        //airbudsPlaylistListView_->get_gameObject()->set_active(true);
    }

    // Set the icon
    static constexpr std::string_view KEY_DL_ICON = "show-downloaded-songs-icon";
    UnityW<UnityEngine::Sprite> sprite = SpriteCache::getInstance().get(KEY_DL_ICON);
    if (!sprite) {
        sprite = BSML::Lite::ArrayToSprite(IncludedAssets::show_downloaded_songs_png);
        SpriteCache::getInstance().add(KEY_DL_ICON, sprite);
    }
    hideDownloadedMapsButton_->GetComponent<BSML::ButtonIconImage*>()->SetIcon(sprite);
    static constexpr float scale = 1.5f;
    hideDownloadedMapsButton_->get_transform()->Find("Content/Icon")->set_localScale({scale, scale, scale});

    setSelectedSongUi(previewSong_);

    Utils::removeRaycastFromButtonIcon(playlistsMenuButton_);
    Utils::removeRaycastFromButtonIcon(randomTrackButton_);
    Utils::removeRaycastFromButtonIcon(showAllByArtistButton_);
    Utils::removeRaycastFromButtonIcon(hideDownloadedMapsButton_);

    BSML::MainThreadScheduler::ScheduleNextFrame([this]() {
        forceLayoutRebuild();
    });
}

void MainViewController::onRandomTrackButtonClicked() {
    auto* trackTableViewDataSource = gameObject->GetComponent<AirbudsTrackTableViewDataSource*>();
    if (!trackTableViewDataSource) {
        return;
    }

    const bool useAllDays = randomAcrossAllDays_ || !selectedPlaylist_;
    if (useAllDays) {
        std::vector<airbuds::PlaylistTrack> allTracks = getRecentlyPlayedCachedOnlyForCurrentUser();
        if (allTracks.empty()) {
            return;
        }

        std::random_device randomDevice;
        std::mt19937 randomGenerator(randomDevice());
        std::uniform_int_distribution<> dist(0, static_cast<int>(allTracks.size() - 1));

        static std::string lastRandomKey;
        size_t attempts = 0;
        airbuds::PlaylistTrack selected = allTracks.at(static_cast<size_t>(dist(randomGenerator)));
        while (allTracks.size() > 1 && makeTrackKey(selected) == lastRandomKey && attempts < 10) {
            selected = allTracks.at(static_cast<size_t>(dist(randomGenerator)));
            ++attempts;
        }
        lastRandomKey = makeTrackKey(selected);

        std::string dayKey;
        if (!getLocalDateKey(selected.dateAdded_, dayKey)) {
            dayKey = "unknown";
        }

        pendingRandomTrack_ = selected;
        if (!selectedPlaylist_ || selectedPlaylist_->id != dayKey) {
            if (!selectPlaylistById(dayKey)) {
                pendingRandomTrack_.reset();
            }
            return;
        }

        const int rowIndex = trackTableViewDataSource->getRowIndexForTrack(selected);
        if (rowIndex >= 0) {
            airbudsTrackListView_->tableView->SelectCellWithIdx(rowIndex, true);
            airbudsTrackListView_->tableView->ScrollToCellWithIdx(rowIndex, HMUI::TableView_ScrollPositionType::Center, true);
        }
        pendingRandomTrack_.reset();
        return;
    }

    if (!selectedPlaylist_) {
        return;
    }

    const size_t trackCount = trackTableViewDataSource->trackCount();
    if (trackCount < 2) {
        return;
    }

    std::random_device randomDevice;
    std::mt19937 randomGenerator(randomDevice());

    std::uniform_int_distribution<> dist(0, trackCount - 1);

    static int lastRandomIndex = -1;

    int index = lastRandomIndex;
    while (index == lastRandomIndex) {
        index = dist(randomGenerator);
    }
    lastRandomIndex = index;

    const int rowIndex = trackTableViewDataSource->getRowIndexForTrackIndex(static_cast<size_t>(index));
    if (rowIndex >= 0) {
        airbudsTrackListView_->tableView->SelectCellWithIdx(rowIndex, true);
        airbudsTrackListView_->tableView->ScrollToCellWithIdx(rowIndex, HMUI::TableView_ScrollPositionType::Center, true);
    }
}

void MainViewController::onRandomScopeToggleClicked() {
    randomAcrossAllDays_ = !randomAcrossAllDays_;
    updateRandomScopeButtonLabel();
}

void MainViewController::onAirbudsTrackListRetryButtonClicked() {
    if (selectedPlaylist_) {
        reloadAirbudsTrackListView();
    } else {
        reloadAirbudsPlaylistListView();
    }
}

void MainViewController::setFilter(const CustomSongFilter& customSongFilter) {
    AirbudsSearch::Log.info("MainViewController::setFilter() called, isShowingDownloadedMaps_={}, input.includeDownloadedSongs_={}", isShowingDownloadedMaps_.load(), customSongFilter.includeDownloadedSongs_);
    customSongFilter_ = customSongFilter;
    customSongFilter_.includeDownloadedSongs_ = isShowingDownloadedMaps_;
    AirbudsSearch::Log.info("MainViewController::setFilter() after assignment, customSongFilter_.includeDownloadedSongs_={}", customSongFilter_.includeDownloadedSongs_);
    if (selectedTrack_) {
        doSongSearch(*selectedTrack_);
    }
}

void MainViewController::setHistoryFriend(const std::optional<airbuds::Friend>& friendUser) {
    if (friendUser && selectedFriend_ && friendUser->id == selectedFriend_->id) {
        return;
    }
    if (!friendUser && !selectedFriend_) {
        return;
    }

    selectedFriend_ = friendUser;
    selectedPlaylist_.reset();
    selectedTrack_.reset();
    pendingRandomTrack_.reset();

    if (airbudsPlaylistListView_) {
        airbudsPlaylistListView_->tableView->ClearSelection();
    }
    if (airbudsTrackListView_) {
        airbudsTrackListView_->tableView->ClearSelection();
    }

    auto* trackTableViewDataSource = gameObject->GetComponent<AirbudsTrackTableViewDataSource*>();
    if (trackTableViewDataSource && airbudsTrackListView_) {
        trackTableViewDataSource->clearTracks();
        airbudsTrackListView_->tableView->ReloadData();
    }

    auto* playlistTableViewDataSource = gameObject->GetComponent<AirbudsPlaylistTableViewDataSource*>();
    if (playlistTableViewDataSource && airbudsPlaylistListView_) {
        playlistTableViewDataSource->playlists_.clear();
        airbudsPlaylistListView_->tableView->ReloadData();
    }

    // Hide the search results
    searchResultsList_->get_gameObject()->set_active(false);
    searchResultsListLoadingIndicatorContainer_->get_gameObject()->set_active(false);
    searchResultsListViewErrorContainer_->get_gameObject()->set_active(false);

    setSelectedSongUi(nullptr);

    showAllByArtistButton_->get_gameObject()->set_active(false);
    hideDownloadedMapsButton_->get_gameObject()->set_active(false);

    updateHistoryTitle();
    playlistsMenuButton_->get_gameObject()->set_active(false);

    reloadAirbudsPlaylistListView();
}

void MainViewController::ctor() {
    previewSong_ = nullptr;
    selectedPlaylist_ = nullptr;
    isDownloadThreadRunning_ = false;
    isLoadingMoreAirbudsTracks_ = false;
    isLoadingMoreAirbudsPlaylists_ = false;
    isShowingAllTracksByArtist_ = false;
    isShowingDownloadedMaps_ = true;
    customSongFilter_ = CustomSongFilter();
    customSongFilter_.includeDownloadedSongs_ = true;
    randomAcrossAllDays_ = false;
    pendingRandomTrack_.reset();
    selectedFriend_.reset();
    currentSongFilter_ = AirbudsSearch::Filter::DEFAULT_SONG_FILTER_FUNCTION;
    currentSongScore_ = AirbudsSearch::Filter::DEFAULT_SONG_SCORE_FUNCTION;
    AirbudsSearch::Log.info("MainViewController::ctor() called, isShowingDownloadedMaps_={}, customSongFilter_.includeDownloadedSongs_={}", isShowingDownloadedMaps_.load(), customSongFilter_.includeDownloadedSongs_);
}

void MainViewController::showAirbudsTrackLoadingIndicator() {
    // Hide the list views
    airbudsTrackListView_->get_gameObject()->set_active(false);
    airbudsPlaylistListView_->get_gameObject()->set_active(false);

    // Hide the error message container
    airbudsListViewErrorContainer_->get_gameObject()->set_active(false);

    // Show the loading indicator
    airbudsTrackListLoadingIndicatorContainer_->get_gameObject()->set_active(true);
    randomTrackButton_->get_gameObject()->set_active(false);
    setRandomScopeVisible(false);
}

void MainViewController::showAirbudsTrackListView() {
    // Hide the error message container
    airbudsListViewErrorContainer_->get_gameObject()->set_active(false);

    // Hide the loading indicator
    airbudsTrackListLoadingIndicatorContainer_->get_gameObject()->set_active(false);

    // Show the playlist list view
    airbudsTrackListView_->get_gameObject()->set_active(true);
    airbudsPlaylistListView_->get_gameObject()->set_active(false);
    setRandomScopeVisible(true);
    forceLayoutRebuild();
}

void MainViewController::showAirbudsPlaylistListView() {
    // Hide the error message container
    airbudsListViewErrorContainer_->get_gameObject()->set_active(false);

    // Hide the loading indicator
    airbudsTrackListLoadingIndicatorContainer_->get_gameObject()->set_active(false);

    // Show the playlist list view
    airbudsTrackListView_->get_gameObject()->set_active(false);
    airbudsPlaylistListView_->get_gameObject()->set_active(true);
    randomTrackButton_->get_gameObject()->set_active(true);
    setRandomScopeVisible(false);
    forceLayoutRebuild();
}

void MainViewController::onAirbudsTrackLoadingError(const std::string& message) {
    // Hide the loading indicator
    airbudsTrackListLoadingIndicatorContainer_->get_gameObject()->set_active(false);

    // Hide the list views
    airbudsTrackListView_->get_gameObject()->set_active(false);
    airbudsPlaylistListView_->get_gameObject()->set_active(false);

    // Show the error message
    airbudsListViewErrorContainer_->get_gameObject()->set_active(true);
    airbudsTrackListErrorMessageTextView_->set_text(message);
    randomTrackButton_->get_gameObject()->set_active(false);
    setRandomScopeVisible(false);
}

void MainViewController::reloadAirbudsPlaylistListView() {
    airbudsListViewStatusContainer_->get_gameObject()->set_active(false);

    showAirbudsTrackLoadingIndicator();
    auto* playlistTableViewDataSource = gameObject->GetComponent<AirbudsPlaylistTableViewDataSource*>();
    const std::optional<std::string> friendId = getSelectedFriendId();
    isLoadingMoreAirbudsPlaylists_ = true;
    std::thread([this, playlistTableViewDataSource, friendId]() {
        // Make sure the Airbuds client is still valid
        if (!AirbudsSearch::airbudsClient) {
            isLoadingMoreAirbudsPlaylists_ = false;
            // TODO: update vis
            return;
        }

        // Load playlists
        std::vector<airbuds::Playlist> playlists;
        std::string statusMessage;
        try {
            if (friendId) {
                playlists = airbudsClient->getPlaylistsForUser(*friendId);
            } else {
                playlists = airbudsClient->getPlaylists();
            }
        } catch (const std::exception& exception) {
            AirbudsSearch::Log.error("Failed loading playlists: {}", exception.what());
            if (friendId) {
                playlists = airbudsClient->getPlaylistsCachedOnlyForUser(*friendId);
            } else {
                playlists = airbudsClient->getPlaylistsCachedOnly();
            }
            if (!playlists.empty()) {
                statusMessage = "Refresh failed; showing cached history.";
            } else {
                isLoadingMoreAirbudsPlaylists_ = false;
                BSML::MainThreadScheduler::Schedule([this, friendId]() {
                    if (!historyContextMatches(friendId)) {
                        return;
                    }
                    onAirbudsTrackLoadingError("Loading Error");
                });
                return;
            }
        }

        if (playlists.empty()) {
            if (friendId) {
                playlists = airbudsClient->getPlaylistsCachedOnlyForUser(*friendId);
            } else {
                playlists = airbudsClient->getPlaylistsCachedOnly();
            }
            if (!playlists.empty()) {
                statusMessage = "Showing cached history.";
            }
        }

        if (statusMessage.empty() && AirbudsSearch::airbudsClient) {
            const std::string warning = AirbudsSearch::airbudsClient->getLastRecentlyPlayedWarning();
            if (!warning.empty()) {
                statusMessage = warning;
            }
        }

        BSML::MainThreadScheduler::Schedule([this, playlistTableViewDataSource, playlists, statusMessage, friendId]() {
            if (!historyContextMatches(friendId)) {
                AirbudsSearch::Log.warn("Ignoring playlist update because the history user changed.");
                isLoadingMoreAirbudsPlaylists_ = false;
                return;
            }
            showAirbudsPlaylistListView();
            playlistTableViewDataSource->playlists_.clear();
            playlistTableViewDataSource->playlists_.insert(
                playlistTableViewDataSource->playlists_.end(),
                playlists.begin(),
                playlists.end());
            Utils::reloadDataKeepingPosition(airbudsPlaylistListView_->tableView);

            airbudsListViewStatusContainer_->get_gameObject()->set_active(false);
            if (playlists.empty()) {
                airbudsListViewStatusContainer_->get_gameObject()->set_active(true);
                airbudsTrackListStatusTextView_->set_text("No history");
                randomTrackButton_->get_gameObject()->set_active(false);
                setRandomScopeVisible(false);
            } else {
                randomTrackButton_->get_gameObject()->set_active(true);
                setRandomScopeVisible(false);
            }

            if (!statusMessage.empty()) {
                airbudsListViewStatusContainer_->get_gameObject()->set_active(true);
                airbudsTrackListStatusTextView_->set_text(statusMessage);
            }

            isLoadingMoreAirbudsPlaylists_ = false;
            forceLayoutRebuild();
        });
    }).detach();
}

void MainViewController::onPlaylistsMenuButtonClicked() {
    selectedPlaylist_ = nullptr;
    selectedTrack_ = nullptr;

    // Hide tracks list
    airbudsTrackListView_->get_gameObject()->set_active(false);

    // Set title
    updateHistoryTitle();

    // Disable button
    playlistsMenuButton_->get_gameObject()->set_active(false);

    // Load playlists
    airbudsPlaylistListView_->tableView->ClearSelection();
    airbudsPlaylistListView_->get_gameObject()->set_active(true);

    // Hide the search results
    searchResultsList_->get_gameObject()->set_active(false);
    searchResultsListLoadingIndicatorContainer_->get_gameObject()->set_active(false);
    searchResultsListViewErrorContainer_->get_gameObject()->set_active(false);

    setSelectedSongUi(nullptr);

    airbudsListViewStatusContainer_->get_gameObject()->set_active(false);

    showAllByArtistButton_->get_gameObject()->set_active(false);
    hideDownloadedMapsButton_->get_gameObject()->set_active(false);

    auto* playlistTableViewDataSource = gameObject->GetComponent<AirbudsPlaylistTableViewDataSource*>();
    const bool hasHistory = playlistTableViewDataSource && !playlistTableViewDataSource->playlists_.empty();
    randomTrackButton_->get_gameObject()->set_active(hasHistory);
    setRandomScopeVisible(false);

    resetListError();
    airbudsTrackListLoadingIndicatorContainer_->get_gameObject()->set_active(false);
}

bool MainViewController::selectPlaylistById(const std::string_view playlistId) {
    auto* playlistTableViewDataSource = gameObject->GetComponent<AirbudsPlaylistTableViewDataSource*>();
    if (!playlistTableViewDataSource) {
        return false;
    }

    const auto& playlists = playlistTableViewDataSource->playlists_;
    for (size_t i = 0; i < playlists.size(); ++i) {
        if (playlists[i].id == playlistId) {
            onPlaylistSelected(airbudsPlaylistListView_->tableView, static_cast<int>(i));
            airbudsPlaylistListView_->tableView->SelectCellWithIdx(static_cast<int>(i), true);
            return true;
        }
    }
    return false;
}

void MainViewController::onPlaylistSelected(UnityW<HMUI::TableView> table, int id) {
    const auto* const playlistTableViewDataSource = gameObject->GetComponent<AirbudsPlaylistTableViewDataSource*>();
    selectedPlaylist_ = std::make_unique<airbuds::Playlist>(playlistTableViewDataSource->playlists_.at(id));
    AirbudsSearch::Log.info("Selected playlist: {}", selectedPlaylist_->id);

    // Hide playlists list
    airbudsPlaylistListView_->get_gameObject()->set_active(false);

    // Enable button
    playlistsMenuButton_->get_gameObject()->set_active(true);

    // Set title
    airbudsColumnTitleTextView_->set_text(selectedPlaylist_->name);

    // Load tracks list
    auto* trackTableViewDataSource = gameObject->GetComponent<AirbudsTrackTableViewDataSource*>();
    if (trackTableViewDataSource) {
        trackTableViewDataSource->clearTracks();
        airbudsTrackListView_->tableView->ReloadData();
        airbudsTrackListView_->tableView->ClearSelection();
    }
    airbudsTrackListView_->get_gameObject()->set_active(true);
    reloadAirbudsTrackListView();
}

void MainViewController::setSelectedSongUi(const SongDetailsCache::Song* const song) {
    if (!song) {
        // Song name
        previewSongNameTextView_->set_text("-");

        // Song author
        previewSongAuthorTextView_->set_text("-");

        // Loading sprite
        const UnityW<UnityEngine::Sprite> placeholderSprite = Utils::getAlbumPlaceholderSprite();
        previewSongImage_->set_sprite(placeholderSprite);

        // Song uploader
        previewSongUploaderTextView_->set_text("-");

        // Song length
        previewSongLengthTextView_->set_text("-");

        // NPS and NJS
        previewSongNPSTextView_->set_text("-");
        previewSongNJSTextView_->set_text("-");

        // Download and Play buttons
        downloadButton_->get_gameObject()->set_active(false);
        playButton_->get_gameObject()->set_active(false);

        UnityW<GlobalNamespace::SongPreviewPlayer> songPreviewPlayer = BSML::Helpers::GetDiContainer()->Resolve<GlobalNamespace::SongPreviewPlayer*>();
        if (songPreviewPlayer) {
            songPreviewPlayer->CrossfadeToDefault();
        }

        return;
    }

    // Try to load the beatmap. It will be null if it is not loaded locally.
    const std::string songHash = song->hash();
    SongCore::SongLoader::CustomBeatmapLevel* beatmap = SongCore::API::Loading::GetLevelByHash(songHash);

    // Song name
    previewSongNameTextView_->set_text(song->songName());

    // Song author
    previewSongAuthorTextView_->set_text(song->songAuthorName());

    // Loading sprite
    const UnityW<UnityEngine::Sprite> placeholderSprite = Utils::getAlbumPlaceholderSprite();
    previewSongImage_->set_sprite(placeholderSprite);

    // Load cover image
    Utils::getCoverImageSprite(songHash, [this, songHash](const UnityW<UnityEngine::Sprite> sprite) {
        // Check if the selected song has changed
        if (!previewSong_ || previewSong_->hash() != songHash) {
            AirbudsSearch::Log.warn("Cancelled sprite update");
            return;
        }

        // Update UI
        if (sprite) {
            previewSongImage_->set_sprite(sprite);
        } else {
            AirbudsSearch::Log.warn("Failed loading cover image for song with hash: {}", songHash);
        }
    });

    // Song uploader
    previewSongUploaderTextView_->set_text(previewSong_->uploaderName());

    // Song length
    const auto minutes = std::chrono::duration_cast<std::chrono::minutes>(previewSong_->songDuration());
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(previewSong_->songDuration() - minutes);
    // Using bolded "Small Colon" because the normal one doesn't render correctly
    previewSongLengthTextView_->set_text(std::format("{:%M<b>\uFE55</b> %S}", previewSong_->songDuration()));

    // NPS and NJS
    uint32_t minNotes = std::numeric_limits<uint32_t>::max();
    uint32_t maxNotes = 0;
    float minNjs = std::numeric_limits<float>::max();
    float maxNjs = 0;
    for (auto& x : *previewSong_) {
        minNotes = std::min(minNotes, x.notes);
        maxNotes = std::max(maxNotes, x.notes);
        minNjs = std::min(minNjs, x.njs);
        maxNjs = std::max(maxNjs, x.njs);
    }
    const float minNotesPerSecond = static_cast<float>(minNotes) / static_cast<float>(previewSong_->songDurationSeconds);
    const float maxNotesPerSecond = static_cast<float>(maxNotes) / static_cast<float>(previewSong_->songDurationSeconds);
    if (std::fabs(maxNotesPerSecond - minNotesPerSecond) <= 1e-6) {
        previewSongNPSTextView_->set_text(std::format("{:.2f}", minNotesPerSecond));
    } else {
        previewSongNPSTextView_->set_text(std::format("{:.2f} - {:.2f}", minNotesPerSecond, maxNotesPerSecond));
    }
    if (fabs(maxNjs - minNjs) <= 1e-6) {
        previewSongNJSTextView_->set_text(std::format("{:.2f}", minNjs));
    } else {
        previewSongNJSTextView_->set_text(std::format("{:.2f} - {:.2f}", minNjs, maxNjs));
    }

    // Download and Play buttons
    if (beatmap) {
        downloadButton_->set_interactable(false);
        downloadButton_->get_gameObject()->set_active(false);
        playButton_->get_gameObject()->set_active(true);
    } else {
        // Check if this song is pending download
        bool isPendingDownload = false;
        HMUI::FlowCoordinator* parentFlow = BSML::Helpers::GetMainFlowCoordinator()->YoungestChildFlowCoordinatorOrSelf();
        auto flow = static_cast<AirbudsSearch::UI::FlowCoordinators::AirbudsSearchFlowCoordinator*>(parentFlow);
        auto* downloadHistoryTableViewDataSource = flow->downloadHistoryViewController_->GetComponent<DownloadHistoryTableViewDataSource*>();
        for (const std::shared_ptr<DownloadHistoryItem>& downloadHistoryItem : downloadHistoryTableViewDataSource->downloadHistoryItems_) {
            if (downloadHistoryItem->song == song) {
                isPendingDownload = true;
            }
        }

        if (isPendingDownload) {
            downloadButton_->set_interactable(false);
            playButton_->get_gameObject()->set_active(false);
        } else {
            downloadButton_->set_interactable(true);
            downloadButton_->get_gameObject()->set_active(true);
            playButton_->get_gameObject()->set_active(false);
        }
    }
}

void MainViewController::doSongSearch(const airbuds::Track& track) {
    AirbudsSearch::Filter::captureMainThreadId();
    const std::string romaji = AirbudsSearch::Filter::getTrackRomajiCached(track);
    const std::vector<AirbudsSearch::Filter::ArtistMatchInfo> artistInfos = AirbudsSearch::Filter::buildArtistInfos(track.artists);
    const std::string artistsRomaji = AirbudsSearch::Filter::getArtistRomajiLog(artistInfos);
    const std::vector<AirbudsSearch::Filter::QuerySpec> queries = AirbudsSearch::Filter::buildBeatSaverQueries(track, artistInfos);
    std::string artistNames;
    for (size_t i = 0; i < track.artists.size(); ++i) {
        if (i > 0) {
            artistNames += ", ";
        }
        artistNames += track.artists[i].name;
    }
    auto difficultyToString = [](SongDetailsCache::MapDifficulty diff) {
        switch (diff) {
            case SongDetailsCache::MapDifficulty::Easy:
                return "Easy";
            case SongDetailsCache::MapDifficulty::Normal:
                return "Normal";
            case SongDetailsCache::MapDifficulty::Hard:
                return "Hard";
            case SongDetailsCache::MapDifficulty::Expert:
                return "Expert";
            case SongDetailsCache::MapDifficulty::ExpertPlus:
                return "ExpertPlus";
            default:
                return "Unknown";
        }
    };
    std::string difficultyList;
    for (size_t i = 0; i < customSongFilter_.difficulties_.size(); ++i) {
        if (i > 0) {
            difficultyList += ",";
        }
        difficultyList += difficultyToString(customSongFilter_.difficulties_[i]);
    }
    AirbudsSearch::Log.info(
        "Searching for track: id={} name=\"{}\" artists=\"{}\" artistsRomaji=\"{}\" romaji=\"{}\"",
        track.id,
        track.name,
        artistNames,
        artistsRomaji,
        romaji);
    if (queries.empty()) {
        AirbudsSearch::Log.warn("BeatSaver search skipped: no query terms available.");
    } else {
        std::string queryLog;
        for (size_t i = 0; i < queries.size(); ++i) {
            if (i > 0) {
                queryLog += " | ";
            }
            queryLog += std::format("[{}:{}] {}", queries[i].label, queries[i].baseScore, queries[i].query);
        }
        AirbudsSearch::Log.info("BeatSaver search queries: {}", queryLog);
    }
    AirbudsSearch::Log.info(
        "Search filter: difficulties=[{}] includeDownloaded={}",
        difficultyList,
        customSongFilter_.includeDownloadedSongs_);

    // Show loading indicator
    searchResultsListLoadingIndicatorContainer_->get_gameObject()->set_active(true);
    searchResultsList_->get_gameObject()->set_active(false);
    searchResultsListViewErrorContainer_->get_gameObject()->set_active(false);

    isSearchInProgress_ = true;
    const CustomSongFilter customSongFilter = customSongFilter_;
    const bool applyArtistBoost = isShowingAllTracksByArtist_;
    std::thread([this, track, romaji, artistInfos, customSongFilter, applyArtistBoost, queries]() {
        SongDetailsCache::SongDetails* songDetails = SongDetailsCache::SongDetails::Init().get();
        if (!songDetails) {
            AirbudsSearch::Log.warn("SongDetails cache is not available yet.");
        }

        const auto searchStartTime = std::chrono::high_resolution_clock::now();
        struct Candidate {
            const SongDetailsCache::Song* song = nullptr;
            int score = 0;
            int rank = 0;
        };
        std::unordered_map<std::string, Candidate> candidates;
        size_t missingInCache = 0;
        size_t duplicateHashes = 0;
        size_t filteredDownloaded = 0;
        size_t mappedByHash = 0;
        size_t mappedByKey = 0;
        size_t mappedById = 0;
        size_t difficultyBonusCount = 0;
        bool hadAnySuccess = false;
        bool hadAnyFailure = false;
        size_t totalDocs = 0;

        for (size_t queryIndex = 0; queryIndex < queries.size(); ++queryIndex) {
            const auto& spec = queries[queryIndex];
            const int page = 0;
            const std::string encodedQuery = AirbudsSearch::Filter::urlEncodeQuery(spec.query);
            const std::string url = std::format(BEATSAVER_API_URL "/search/text/{}?q={}&sortOrder=Relevance", page, encodedQuery);
            WebUtils::URLOptions urlOptions(url);
            urlOptions.noEscape = true;
            AirbudsSearch::Log.info(
                "BeatSaver request: Search url={} label={} query=\"{}\" encoded=\"{}\"",
                urlOptions.fullURl(),
                spec.label,
                spec.query,
                encodedQuery);
            auto response = WebUtils::Get<BeatSaver::API::SearchPageResponse>(urlOptions);
            AirbudsSearch::Log.info(
                "BeatSaver response: Search http={} curl={} parsed={} hasData={}",
                response.get_HttpCode(),
                response.get_CurlStatus(),
                response.DataParsedSuccessful(),
                response.responseData.has_value());

            if (!response.IsSuccessful() || !response.responseData) {
                hadAnyFailure = true;
                continue;
            }

            hadAnySuccess = true;
            const auto& docs = response.responseData->Docs;
            totalDocs += docs.size();
            AirbudsSearch::Log.info("BeatSaver search results: query=\"{}\" count={}", spec.query, docs.size());
            for (size_t docIndex = 0; docIndex < docs.size(); ++docIndex) {
                const BeatSaver::Models::Beatmap& beatmap = docs[docIndex];
                const auto versions = beatmap.Versions;
                if (versions.empty()) {
                    continue;
                }
                const BeatSaver::Models::BeatmapVersion& version = versions.front();
                const std::string versionHash = version.Hash;

                const SongDetailsCache::Song* song = nullptr;
                bool mapped = false;
                if (songDetails && songDetails->songs.FindByHash(versionHash, song) && song) {
                    mapped = true;
                    ++mappedByHash;
                } else if (songDetails) {
                    if (version.Key && songDetails->songs.FindByMapId(*version.Key, song) && song) {
                        mapped = true;
                        ++mappedByKey;
                    } else if (!beatmap.Id.empty() && songDetails->songs.FindByMapId(beatmap.Id, song) && song) {
                        mapped = true;
                        ++mappedById;
                    }
                }
                if (!mapped || !song) {
                    ++missingInCache;
                    continue;
                }

                const std::string songHash = song->hash();
                if (!customSongFilter.includeDownloadedSongs_ && SongCore::API::Loading::GetLevelByHash(songHash)) {
                    ++filteredDownloaded;
                    continue;
                }

                const bool hasDifficultyBonus = AirbudsSearch::Filter::beatmapHasDifficulty(version, customSongFilter.difficulties_);
                if (hasDifficultyBonus) {
                    ++difficultyBonusCount;
                }
                const int matchScore = AirbudsSearch::Filter::scoreBeatSaverCandidate(track, romaji, artistInfos, beatmap, applyArtistBoost, hasDifficultyBonus);
                const int score = spec.baseScore + matchScore - static_cast<int>(docIndex);
                const int rank = static_cast<int>(queryIndex * 100 + docIndex);

                auto it = candidates.find(songHash);
                if (it == candidates.end()) {
                    candidates.emplace(songHash, Candidate{song, score, rank});
                } else {
                    ++duplicateHashes;
                    Candidate& existing = it->second;
                    if (score > existing.score || (score == existing.score && rank < existing.rank)) {
                        existing.song = song;
                        existing.score = score;
                        existing.rank = rank;
                    }
                }
            }
        }

        std::vector<Candidate> sortedCandidates;
        sortedCandidates.reserve(candidates.size());
        for (const auto& [hash, candidate] : candidates) {
            if (candidate.song) {
                sortedCandidates.push_back(candidate);
            }
        }

        AirbudsSearch::Log.info(
            "Online search mapped songs = {} candidates={} totalDocs={} missingInCache={} duplicates={} filteredDownloaded={} difficultyBonus={} mappedByHash={} mappedByKey={} mappedById={} time = {} ms.",
            sortedCandidates.size(),
            candidates.size(),
            totalDocs,
            missingInCache,
            duplicateHashes,
            filteredDownloaded,
            difficultyBonusCount,
            mappedByHash,
            mappedByKey,
            mappedById,
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::high_resolution_clock::now() - searchStartTime).count());

        if (hadAnyFailure && !hadAnySuccess) {
            AirbudsSearch::Log.warn("All BeatSaver search requests failed.");
        }

        // Sort results
        auto sortStartTime = std::chrono::high_resolution_clock::now();
        std::stable_sort(sortedCandidates.begin(), sortedCandidates.end(), [](const Candidate& a, const Candidate& b) {
            if (a.score != b.score) {
                return a.score > b.score;
            }
            return a.rank < b.rank;
        });
        AirbudsSearch::Log.info("Sort time: {} ms.", std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::high_resolution_clock::now() - sortStartTime).count());

        std::vector<const SongDetailsCache::Song*> songs;
        songs.reserve(sortedCandidates.size());
        for (const Candidate& candidate : sortedCandidates) {
            songs.push_back(candidate.song);
        }

        const size_t previewCount = std::min<size_t>(5, sortedCandidates.size());
        for (size_t i = 0; i < previewCount; ++i) {
            const Candidate& candidate = sortedCandidates.at(i);
            const SongDetailsCache::Song* song = candidate.song;
            AirbudsSearch::Log.info(
                "Search result[{}]: score={} name=\"{}\" author=\"{}\" hash={}",
                i,
                candidate.score,
                song->songName(),
                song->songAuthorName(),
                song->hash());
        }

        const bool showSearchError = !queries.empty() && !hadAnySuccess;
        BSML::MainThreadScheduler::Schedule([this, track, songs, showSearchError]() {
            // Check if the user canceled the selection
            if (selectedTrack_ == nullptr) {
                AirbudsSearch::Log.warn("Ignoring search results because the selected track is null!");
                isSearchInProgress_ = false;
                return;
            }

            // Check if the user selected a different track
            if (*selectedTrack_ != track) {
                AirbudsSearch::Log.warn("Ignoring search results because the selected track has changed! (requested = {} / current = {})", track.id, selectedTrack_->id);
                isSearchInProgress_ = false;
                return;
            }

            // Update the list view
            auto customSongTableViewDataSource = gameObject->GetComponent<CustomSongTableViewDataSource*>();
            customSongTableViewDataSource->setSource(songs);
            searchResultsList_->tableView->SetDataSource(reinterpret_cast<HMUI::TableView::IDataSource*>(customSongTableViewDataSource), true);
            searchResultsList_->tableView->ClearSelection();

            searchResultsListLoadingIndicatorContainer_->get_gameObject()->set_active(false);
            searchResultsList_->get_gameObject()->set_active(true);

            if (songs.empty()) {
                searchResultsListViewErrorContainer_->get_gameObject()->set_active(true);
                searchResultsList_->get_gameObject()->set_active(false);
                searchResultsListStatusTextView_->set_text(showSearchError ? "Search Error" : "No Songs");
            }

            searchResultItems_ = songs;

            isSearchInProgress_ = false;

            // Automatically select the first search result
            if (!searchResultItems_.empty()) {
                searchResultsList_->tableView->SelectCellWithIdx(0, true);
            }
        });
    }).detach();
}

void MainViewController::onTrackSelected(UnityW<HMUI::TableView> table, int id) {
    // Clear UI
    setSelectedSongUi(nullptr);

    // Get the selected track
    const AirbudsTrackTableViewDataSource* const trackTableViewDataSource = gameObject->GetComponent<AirbudsTrackTableViewDataSource*>();
    const airbuds::PlaylistTrack* track = trackTableViewDataSource->getTrackForRow(id);
    if (!track) {
        return;
    }
    selectedTrack_ = std::make_unique<const airbuds::Track>(*track);

    // Start search
    AirbudsSearch::Log.info("MainViewController::onTrackSelected() before doSongSearch, isShowingDownloadedMaps_={}, customSongFilter_.includeDownloadedSongs_={}", isShowingDownloadedMaps_.load(), customSongFilter_.includeDownloadedSongs_);
    doSongSearch(*track);

    // Enable the artist search button
    showAllByArtistButton_->get_gameObject()->set_active(true);
    hideDownloadedMapsButton_->get_gameObject()->set_active(true);

    // Update the artist search button
    if (isShowingAllTracksByArtist_) {
        std::stringstream stringStream;
        stringStream << "Showing all songs by";
        for (const airbuds::Artist& artist : selectedTrack_->artists) {
            stringStream << "\n<color=blue>" << artist.name << "</color>";
        }
        UnityW<HMUI::HoverHint> hoverHintComponent = showAllByArtistButton_->GetComponent<HMUI::HoverHint*>();
        hoverHintComponent->set_text(stringStream.str());

        UnityW<HMUI::ImageView> iconImageView = showAllByArtistButton_->get_transform()->Find("Content/Icon")->GetComponent<HMUI::ImageView*>();
        UnityW<HMUI::ImageView> underlineImageView = showAllByArtistButton_->get_transform()->Find("Underline")->GetComponent<HMUI::ImageView*>();
        const UnityEngine::Color color(0.0f, 0.8118f, 1.0f, 1.0f);
        iconImageView->set_color(color);
        underlineImageView->set_color(color);
    }
}

void MainViewController::onSearchResultSelected(UnityW<HMUI::TableView> table, int id) {
    previewSong_ = searchResultItems_.at(id);

    setSelectedSongUi(previewSong_);

    // Try to load the beatmap. It will be null if it is not loaded locally.
    const std::string songHash = previewSong_->hash();
    SongCore::SongLoader::CustomBeatmapLevel* beatmap = SongCore::API::Loading::GetLevelByHash(songHash);

    // Get audio preview
    if (beatmap) {
        auto* levelCollectionViewController = BSML::Helpers::GetDiContainer()->Resolve<GlobalNamespace::LevelCollectionViewController*>();
        levelCollectionViewController->SongPlayerCrossfadeToLevelAsync(beatmap, System::Threading::CancellationToken::get_None());
    } else {
        Utils::getAudioClipForSongHash(songHash, [this, songHash](UnityW<UnityEngine::AudioClip> audioClip) {
            // Check if the selected song has changed
            if (!previewSong_ || previewSong_->hash() != songHash) {
                AirbudsSearch::Log.warn("Cancelled audio update");
                UnityEngine::Object::Destroy(audioClip);
                return;
            }

            const std::function<void()> onFadeOutCallback = [audioClip]() {
                if (!audioClip) {
                    return;
                }
                try {
                    UnityEngine::Object::Destroy(audioClip);
                } catch (...) {
                    AirbudsSearch::Log.error("Error destroying clip");
                }
            };

            auto* spp = BSML::Helpers::GetDiContainer()->Resolve<GlobalNamespace::SongPreviewPlayer*>();
            spp->CrossfadeTo(audioClip, -5, 0, audioClip->length, BSML::MakeDelegate<System::Action*>(onFadeOutCallback));
        });
    }
}

void MainViewController::onPlayButtonClicked() {
    AirbudsSearch::Utils::goToLevelSelect(previewSong_->hash());
}

void MainViewController::startDownloadThread() {
    if (isDownloadThreadRunning_) {
        return;
    }
    isDownloadThreadRunning_ = true;
    std::thread([this]() {
        // Process download queue
        while (true) {
            // Get next item in queue
            std::shared_ptr<DownloadHistoryItem> downloadHistoryItem = nullptr;
            {
                std::unique_lock lock(pendingDownloadsMutex_);
                if (pendingDownloads_.empty()) {
                    break;
                }
                downloadHistoryItem = pendingDownloads_.front();
                pendingDownloads_.pop();
            }

            const std::string songHash = downloadHistoryItem->song->hash();

            downloadHistoryItem->onDownloadStarted();

            // Download beatmap info
            const WebUtils::URLOptions beatmapInfoOptions = BeatSaver::API::GetBeatmapByHashURLOptions(songHash);
            AirbudsSearch::Log.info(
                "BeatSaver request: GetBeatmapByHash url={} hash={}",
                beatmapInfoOptions.fullURl(),
                songHash);
            auto response = WebUtils::Get<BeatSaver::API::BeatmapResponse>(beatmapInfoOptions);
            AirbudsSearch::Log.info(
                "BeatSaver response: GetBeatmapByHash http={} curl={} parsed={} hasData={}",
                response.get_HttpCode(),
                response.get_CurlStatus(),
                response.DataParsedSuccessful(),
                response.responseData.has_value());
            if (!response.IsSuccessful()) {
                AirbudsSearch::Log.info(
                    "Failed to download beatmap info for song with hash {} (http={} curl={})",
                    songHash,
                    response.get_HttpCode(),
                    response.get_CurlStatus());
                downloadHistoryItem->onDownloadStopped(false);
                continue;
            }

            // Get beatmap info
            const std::optional<BeatSaver::Models::Beatmap> beatmap = response.responseData;
            if (!beatmap) {
                AirbudsSearch::Log.info(
                    "Empty response when downloading beatmap for song with hash {} (http={} curl={} parsed={})",
                    songHash,
                    response.get_HttpCode(),
                    response.get_CurlStatus(),
                    response.DataParsedSuccessful());
                downloadHistoryItem->onDownloadStopped(false);
                continue;
            }
            AirbudsSearch::Log.info(
                "BeatSaver beatmap: id={} name={} song={} author={} versions={}",
                beatmap->Id,
                beatmap->Name,
                beatmap->Metadata.SongName,
                beatmap->Metadata.LevelAuthorName,
                beatmap->Versions.size());

            // Create download progress callback
            const std::function<void(float)> onDownloadProgress = [downloadHistoryItem](const float progress) {
                downloadHistoryItem->onDownloadProgress(progress);
            };

            // Download beatmap
            const BeatSaver::API::BeatmapDownloadInfo beatmapDownloadInfo(*beatmap);
            const std::pair<WebUtils::URLOptions, BeatSaver::API::DownloadBeatmapResponse> urlOptionsAndResponse = BeatSaver::API::DownloadBeatmapURLOptionsAndResponse(beatmapDownloadInfo);
            BeatSaver::API::DownloadBeatmapResponse downloadBeatmapResponse = urlOptionsAndResponse.second;
            AirbudsSearch::Log.info(
                "BeatSaver request: DownloadBeatmap url={} key={} hash={}",
                urlOptionsAndResponse.first.fullURl(),
                beatmapDownloadInfo.Key,
                songHash);
            BeatSaver::API::GetBeatsaverDownloader().GetInto(urlOptionsAndResponse.first, &downloadBeatmapResponse, onDownloadProgress);
            const bool isSuccessful = downloadBeatmapResponse.IsSuccessful();
            AirbudsSearch::Log.info(
                "BeatSaver response: DownloadBeatmap http={} curl={} success={} output={}",
                downloadBeatmapResponse.get_HttpCode(),
                downloadBeatmapResponse.get_CurlStatus(),
                isSuccessful,
                downloadBeatmapResponse.responseData ? downloadBeatmapResponse.responseData->string() : "");

            downloadHistoryItem->onDownloadStopped(isSuccessful);

            if (!isSuccessful) {
                AirbudsSearch::Log.info(
                    "Failed to download beatmap for song with hash {} (http={} curl={})",
                    songHash,
                    downloadBeatmapResponse.get_HttpCode(),
                    downloadBeatmapResponse.get_CurlStatus());
                continue;
            }
        }

        // Refresh songs
        std::shared_future<void> future = SongCore::API::Loading::RefreshSongs(false);
        future.wait();
        isDownloadThreadRunning_ = false;

        // Refreshing songs can take a while, so if we've requested more downloads since then, start up the thread again
        {
            std::lock_guard lock(pendingDownloadsMutex_);
            if (!pendingDownloads_.empty()) {
                startDownloadThread();
            }
        }

        // Update UI
        BSML::MainThreadScheduler::Schedule([this]() {
            setSelectedSongUi(previewSong_);

            UnityW<HMUI::FlowCoordinator> parentFlowCoordinator = BSML::Helpers::GetMainFlowCoordinator()->YoungestChildFlowCoordinatorOrSelf();
            auto flow = parentFlowCoordinator.cast<AirbudsSearch::UI::FlowCoordinators::AirbudsSearchFlowCoordinator>();
            Utils::reloadDataKeepingPosition(flow->downloadHistoryViewController_->customSongsList_->tableView);

            // Update the search results for green
            Utils::reloadDataKeepingPosition(searchResultsList_->tableView);
        });
    }).detach();
}

bool containsContiguous(const std::vector<std::string>& haystack, const std::vector<std::string>& needle) {
    if (needle.empty()) return true;
    if (needle.size() > haystack.size()) return false;

    for (size_t i = 0; i <= haystack.size() - needle.size(); ++i) {
        bool match = true;
        for (size_t j = 0; j < needle.size(); ++j) {
            if (haystack[i + j] != needle[j]) {
                match = false;
                break;
            }
        }
        if (match) return true;
    }
    return false;
}

void MainViewController::onShowAllByArtistButtonClicked() {
    isShowingAllTracksByArtist_ = !isShowingAllTracksByArtist_;

    UnityW<HMUI::HoverHint> hoverHintComponent = showAllByArtistButton_->GetComponent<HMUI::HoverHint*>();
    UnityW<HMUI::ImageView> iconImageView = showAllByArtistButton_->get_transform()->Find("Content/Icon")->GetComponent<HMUI::ImageView*>();
    UnityW<HMUI::ImageView> underlineImageView = showAllByArtistButton_->get_transform()->Find("Underline")->GetComponent<HMUI::ImageView*>();

    if (isShowingAllTracksByArtist_) {

        // Update the artist search button
        std::stringstream stringStream;
        stringStream << "Showing all songs by";
        for (const airbuds::Artist& artist : selectedTrack_->artists) {
            stringStream << "\n<color=blue>" << artist.name << "</color>";
        }
        hoverHintComponent->set_text(stringStream.str());
        const UnityEngine::Color color(0.0f, 0.8118f, 1.0f, 1.0f);
        iconImageView->set_color(color);
        underlineImageView->set_color(color);

        currentSongFilter_ = [](const SongDetailsCache::Song* const song, const airbuds::Track& track) {
            // Remove songs that don't have at least one word from the Airbuds track in the name
            bool didContainArtist = false;
            for (const airbuds::Artist& artist : track.artists) {
                const std::vector<std::string> artistTokens = AirbudsSearch::Filter::getWords(artist.name);
                if (containsContiguous(AirbudsSearch::Filter::getWords(song->songAuthorName()), artistTokens)) {
                    didContainArtist = true;
                    break;
                }
                if (containsContiguous(AirbudsSearch::Filter::getWords(song->levelAuthorName()), artistTokens)) {
                    didContainArtist = true;
                    break;
                }

                // todo: handle case where song name contains artist but not correct
                // todo: ex. artist = Luma; song name = "Luma Pools"
                //if (containsContiguous(AirbudsSearch::Filter::getWords(song->songName()), artistTokens)) {
                //    didContainArtist = true;
                //    break;
                //}
            }
            return didContainArtist;
        };
        currentSongScore_ = [](const airbuds::Track& track, const SongDetailsCache::Song& song) {
            int score = 0;

            // Increase score based on upvotes
            score += song.upvotes;

            // Decrease score based on downvotes
            score -= song.downvotes;

            score += song.upvotes + song.downvotes;

            return score;
        };
    } else {
        // Update the artist search button
        hoverHintComponent->set_text("Show all songs by this artist");
        iconImageView->set_color(UnityEngine::Color::get_white());
        underlineImageView->set_color(UnityEngine::Color::get_white());

        currentSongFilter_ = AirbudsSearch::Filter::DEFAULT_SONG_FILTER_FUNCTION;
        currentSongScore_ = AirbudsSearch::Filter::DEFAULT_SONG_SCORE_FUNCTION;
    }

    // Hide and show the hover hint to update the text
    auto* controller = UnityEngine::Object::FindObjectOfType<HMUI::HoverHintController*>();
    controller->HideHintInstant(hoverHintComponent);
    controller->SetupAndShowHintPanel(hoverHintComponent);

    doSongSearch(*selectedTrack_);
}

void MainViewController::onHideDownloadedMapsButtonClicked() {
    isShowingDownloadedMaps_ = !isShowingDownloadedMaps_;

    UnityW<HMUI::HoverHint> hoverHintComponent = hideDownloadedMapsButton_->GetComponent<HMUI::HoverHint*>();
    UnityW<HMUI::ImageView> iconImageView = hideDownloadedMapsButton_->get_transform()->Find("Content/Icon")->GetComponent<HMUI::ImageView*>();
    UnityW<HMUI::ImageView> underlineImageView = hideDownloadedMapsButton_->get_transform()->Find("Underline")->GetComponent<HMUI::ImageView*>();

    if (!isShowingDownloadedMaps_) {

        // Update the artist search button
        hoverHintComponent->set_text("Downloaded maps hidden");
        const UnityEngine::Color color(0.0f, 0.8118f, 1.0f, 1.0f);
        iconImageView->set_color(color);
        underlineImageView->set_color(color);
    } else {
        // Update the artist search button
        hoverHintComponent->set_text("Hide downloaded maps");
        iconImageView->set_color(UnityEngine::Color::get_white());
        underlineImageView->set_color(UnityEngine::Color::get_white());
    }

    // Hide and show the hover hint to update the text
    auto* controller = UnityEngine::Object::FindObjectOfType<HMUI::HoverHintController*>();
    controller->HideHintInstant(hoverHintComponent);
    controller->SetupAndShowHintPanel(hoverHintComponent);

    customSongFilter_.includeDownloadedSongs_ = isShowingDownloadedMaps_;
    setFilter(customSongFilter_);
}

void MainViewController::onDownloadButtonClicked() {
    // Add the selected song to the download queue
    const SongDetailsCache::Song* const song = previewSong_;
    const std::shared_ptr<DownloadHistoryItem> downloadHistoryItem = std::make_shared<DownloadHistoryItem>();
    downloadHistoryItem->song = song;
    {
        std::lock_guard lock(pendingDownloadsMutex_);
        pendingDownloads_.push(downloadHistoryItem);
    }

    // Start download thread if needed
    startDownloadThread();

    // Update UI
    UnityW<HMUI::FlowCoordinator> parentFlowCoordinator = BSML::Helpers::GetMainFlowCoordinator()->YoungestChildFlowCoordinatorOrSelf();
    auto flow = parentFlowCoordinator.cast<AirbudsSearch::UI::FlowCoordinators::AirbudsSearchFlowCoordinator>();
    auto* downloadHistoryTableViewDataSource = flow->downloadHistoryViewController_->GetComponent<DownloadHistoryTableViewDataSource*>();
    downloadHistoryTableViewDataSource->downloadHistoryItems_.push_back(downloadHistoryItem);
    flow->downloadHistoryViewController_->customSongsList_->tableView->SetDataSource(reinterpret_cast<HMUI::TableView::IDataSource*>(downloadHistoryTableViewDataSource), true);

    downloadButton_->set_interactable(false);
}
