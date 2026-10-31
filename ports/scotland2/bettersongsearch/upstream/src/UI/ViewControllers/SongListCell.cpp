#include "UI/ViewControllers/SongListCell.hpp"

#include <fmt/chrono.h>
#include <fmt/core.h>

#include "DataHolder.hpp"
#include "logging.hpp"
#include "PluginConfig.hpp"
#include "sombrero/shared/FastColor.hpp"
#include "song-details/shared/Data/MapCharacteristic.hpp"
#include "songcore/shared/SongLoader/RuntimeSongLoader.hpp"
#include "UI/FlowCoordinators/BetterSongSearchFlowCoordinator.hpp"
#include "UnityEngine/Color.hpp"
#include "Util/SongUtil.hpp"

using namespace BetterSongSearch::Util;
using namespace SongDetailsCache;
DEFINE_TYPE(BetterSongSearch::UI::ViewControllers, CustomSongListTableCell);

struct DiffIndex {
    SongDetailsCache::SongDifficulty const* diff;
    bool passesFilter;
    float stars;
};

namespace BetterSongSearch::UI::ViewControllers {
    static std::map<SongDetailsCache::MapDifficulty const, std::string> shortMapDiffNames = {
        {SongDetailsCache::MapDifficulty::Easy, "Easy"},
        {SongDetailsCache::MapDifficulty::Normal, "Norm"},
        {SongDetailsCache::MapDifficulty::Hard, "Hard"},
        {SongDetailsCache::MapDifficulty::Expert, "Ex"},
        {SongDetailsCache::MapDifficulty::ExpertPlus, "Ex+"}
    };

    static std::map<SongDetailsCache::MapCharacteristic, std::string> customCharNames = {
        {SongDetailsCache::MapCharacteristic::Standard, ""},
        {SongDetailsCache::MapCharacteristic::NinetyDegree, "90"},
        {SongDetailsCache::MapCharacteristic::OneSaber, "OS"},
        {SongDetailsCache::MapCharacteristic::ThreeSixtyDegree, "360"},
        {SongDetailsCache::MapCharacteristic::Lawless, "LL"},
        {SongDetailsCache::MapCharacteristic::Custom, "?"},
        {SongDetailsCache::MapCharacteristic::LightShow, "LS"}
    };

    std::string GetCombinedShortDiffName(int diffCount, SongDetailsCache::SongDifficulty const* diff) {
        auto retVal = fmt::format("{}", shortMapDiffNames[diff->difficulty]);

        if (diff->characteristic == SongDetailsCache::MapCharacteristic::Standard) {
            return retVal;
        }
        retVal += fmt::format("({})", customCharNames[diff->characteristic]);

        return retVal;
    }

    void CustomSongListTableCell::ctor() {
        // Subscribe to events
        SongCore::SongLoader::RuntimeSongLoader::get_instance()->SongsLoaded += {&CustomSongListTableCell::OnSongsLoaded, this};

        // Make sure to call the base constructor, otherwise unpredictable things might happen in the next updates
        INVOKE_BASE_CTOR(classof(HMUI::TableCell*));
    }

    void CustomSongListTableCell::OnDestroy() {
        // Unsub from events
        // The only way this can be destroyed is if the game is closing,
        // so no need to unsub, since songcore will be destroyed too
    }

    void CustomSongListTableCell::OnSongsLoaded(std::span<SongCore::SongLoader::CustomBeatmapLevel* const> songs) {
        if (!this->get_isActiveAndEnabled()) {
            return;
        }
        if (!dataHolder.songDetails->songs.get_isDataAvailable()) {
            return;
        }
        if (!this->entry) {
            return;
        }

        // Refresh the state
        PopulateWithSongData(this->entry);
    }

    CustomSongListTableCell* CustomSongListTableCell::PopulateWithSongData(SongDetailsCache::Song const* entry) {
        if (!entry) {
            WARNING("Tried to populate with null entry");
            return this;
        }

        // Colors
        static auto verifiedSongColor = Sombrero::FastColor(.7f, 1.0f, .7f, 1.0f);
        static auto verifiedUploaderColor = Sombrero::FastColor(.46f, .27f, .68f, 1.0f);
        static auto normalUploaderColor = Sombrero::FastColor(.8f, .8f, .8f, 1.0f);

        bool isCurated = hasFlags(entry->uploadFlags, SongDetailsCache::UploadFlags::Curated);

        this->levelAuthorName->set_text(entry->levelAuthorName());
        this->songLengthAndRating->set_text(fmt::format(
            "Length: {:%M:%S} Upvotes: {}, Downvotes: {}", std::chrono::seconds(entry->songDurationSeconds), entry->upvotes, entry->downvotes
        ));
        this->uploadDateFormatted->set_text(fmt::format("{:%d. %b %Y}", fmt::localtime(entry->uploadTimeUnix)));
        bool isDownloaded = fcInstance->DownloadHistoryViewController->CheckIsDownloaded(entry->hash());

        // Song name color
        Sombrero::FastColor songColor = Sombrero::FastColor::white();
        if (isDownloaded) {
            songColor = Sombrero::FastColor(0.53f, 0.53f, 0.53f, 1.0f);
        } else {
            if (isCurated) {
                songColor = verifiedSongColor;
            }
        }
        this->fullFormattedSongName->set_text(fmt::format("{} - {}", entry->songAuthorName(), entry->songName()));
        this->fullFormattedSongName->set_color(songColor);

        // Author color
        bool isVerified = hasFlags(entry->uploadFlags, SongDetailsCache::UploadFlags::VerifiedUploader);
        this->levelAuthorName->set_color(isVerified ? verifiedUploaderColor : normalUploaderColor);

        this->entry = entry;

        std::vector<DiffIndex> sortedDiffs;
        for (SongDetailsCache::SongDifficulty const& diff : *entry) {
            sortedDiffs.push_back({&diff, DifficultyCheck(&diff, entry), getStars(&diff)});
        };

        // TODO: Actually sort diffs..
        std::stable_sort(sortedDiffs.begin(), sortedDiffs.end(), [entry](DiffIndex const& a, DiffIndex const& b) {
            auto diff1 = (a.passesFilter ? 1 : -3) + (a.diff->characteristic == SongDetailsCache::MapCharacteristic::Standard ? 1 : 0);
            auto diff2 = (b.passesFilter ? 1 : -3) + (b.diff->characteristic == SongDetailsCache::MapCharacteristic::Standard ? 1 : 0);

            // Descending
            return diff1 > diff2;
        });

        // If most stars
        if (dataHolder.currentSort == FilterTypes::SortMode::Most_Stars) {
            std::stable_sort(sortedDiffs.begin(), sortedDiffs.end(), [entry](DiffIndex const& a, DiffIndex const& b) {
                auto diff1 = -a.stars;
                auto diff2 = -b.stars;

                // Ascending
                return diff1 < diff2;
            });
        }
        // If least stars
        if (dataHolder.currentSort == FilterTypes::SortMode::Least_Stars) {
            std::stable_sort(sortedDiffs.begin(), sortedDiffs.end(), [entry](DiffIndex const& a, DiffIndex const& b) {
                auto diff1 = a.stars > 0 ? a.stars : -420.0f;
                auto diff2 = b.stars > 0 ? b.stars : -420.0f;
                // Ascending
                return diff1 < diff2;
            });
        }

        // By ranked
        std::stable_sort(sortedDiffs.begin(), sortedDiffs.end(), [entry](DiffIndex const& a, DiffIndex const& b) {
            auto diff1 = a.stars > 0 ? 1 : 0;
            auto diff2 = b.stars > 0 ? 1 : 0;
            // Descending
            return diff1 < diff2;
        });

        int diffsLeft = sortedDiffs.size();

        for (int i = 0; i < diffs.size(); i++) {
            bool isActive = diffsLeft != 0;
            diffs[i]->get_gameObject()->set_active(isActive);
            if (!isActive) {
                continue;
            }
            if (diffsLeft != 1 && i == diffs.size() - 1) {
                diffs[i]->set_text(fmt::format("<color=#0AD>{} More", diffsLeft));
            } else {
                bool passesFilter = sortedDiffs[i].passesFilter;
                auto diffname = GetCombinedShortDiffName(entry->diffCount, sortedDiffs[i].diff);
                std::string stars = "";

                auto lbsvc = GetTargetedRankLeaderboardService(sortedDiffs[i].diff);

                if (lbsvc == RankedStates::ScoresaberRanked && sortedDiffs[i].diff->starsSS > 0) {
                    stars = fmt::format(" <color=#{}>{}", (passesFilter ? "D91" : "650"), fmt::format("{:.{}f}", sortedDiffs[i].stars, 1));
                } else if (lbsvc == RankedStates::BeatleaderRanked && sortedDiffs[i].diff->starsBL > 0) {
                    stars = fmt::format(" <color=#{}>{}", (passesFilter ? "B1D" : "606"), fmt::format("{:.{}f}", sortedDiffs[i].stars, 1));
                }
                auto text = fmt::format("<color=#{}>{}</color>{}", (passesFilter ? "EEE" : "888"), diffname, stars);
                diffs[i]->SetText(text, false);
                diffsLeft--;
            }
        }

        SetFontSizes();
        return this;
    }

    void CustomSongListTableCell::RefreshBgState() {
        bgContainer->set_color(UnityEngine::Color(0, 0, 0, selected || highlighted ? 0.8f : 0.45f));
    }

    void CustomSongListTableCell::SelectionDidChange(HMUI::SelectableCell::TransitionType transitionType) {
        RefreshBgState();
    }

    void CustomSongListTableCell::HighlightDidChange(HMUI::SelectableCell::TransitionType transitionType) {
        RefreshBgState();
    }

    void CustomSongListTableCell::WasPreparedForReuse() {
        entry = nullptr;
    }

    void CustomSongListTableCell::SetFontSizes() {
        bool smallFont = getPluginConfig().SmallerFontSize.GetValue();
        for (auto d : diffs) {
            d->set_fontSize(smallFont ? 2.5f : 2.9f);
        }

        // TODO: Different font sizes
        if (smallFont) {
            fullFormattedSongName->set_fontSize(2.7f);
            uploadDateFormatted->set_fontSize(2.7f);
            levelAuthorName->set_fontSize(2.3f);
            songLengthAndRating->set_fontSize(2.5f);
        } else {
            fullFormattedSongName->set_fontSize(3.2f);
            uploadDateFormatted->set_fontSize(3.2f);
            levelAuthorName->set_fontSize(2.6f);
            songLengthAndRating->set_fontSize(3.0f);
        }

        return;
    };
}  // namespace BetterSongSearch::UI::ViewControllers
