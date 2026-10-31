#include <chrono>
#include <cstdio>
#include <ctime>

#include "HMUI/Touchable.hpp"
#include "bsml/shared/BSML/MainThreadScheduler.hpp"

#include "assets.hpp"
#include "Log.hpp"
#include "UI/TableViewCells/AirbudsTrackHeaderTableViewCell.hpp"
#include "UI/TableViewCells/AirbudsTrackTableViewCell.hpp"
#include "UI/TableViewDataSources/AirbudsTrackTableViewDataSource.hpp"
#include "main.hpp"

DEFINE_TYPE(AirbudsSearch::UI, AirbudsTrackTableViewDataSource);

using namespace AirbudsSearch::UI;

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

bool getLocalHourKey(const std::chrono::milliseconds& millis, std::string& key, std::string& label) {
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

    char keyBuffer[24];
    std::snprintf(
        keyBuffer,
        sizeof(keyBuffer),
        "%04d-%02d-%02d %02d",
        localTime.tm_year + 1900,
        localTime.tm_mon + 1,
        localTime.tm_mday,
        localTime.tm_hour);
    key = keyBuffer;

    char labelBuffer[8];
    std::snprintf(labelBuffer, sizeof(labelBuffer), "%02d:00", localTime.tm_hour);
    label = labelBuffer;
    return true;
}

std::string getTodayKey() {
    const std::time_t now = std::time(nullptr);
    std::tm localTime{};
#if defined(_WIN32)
    localtime_s(&localTime, &now);
#else
    localtime_r(&now, &localTime);
#endif

    char buffer[16];
    std::snprintf(
        buffer,
        sizeof(buffer),
        "%04d-%02d-%02d",
        localTime.tm_year + 1900,
        localTime.tm_mon + 1,
        localTime.tm_mday);
    return buffer;
}

bool tracksMatch(const airbuds::PlaylistTrack& left, const airbuds::PlaylistTrack& right) {
    if (left.id != right.id) {
        return false;
    }
    const auto leftMillis = left.dateAdded_.count();
    const auto rightMillis = right.dateAdded_.count();
    if (leftMillis > 0 || rightMillis > 0) {
        return leftMillis == rightMillis;
    }
    return left.dateAdded == right.dateAdded;
}

}

HMUI::TableCell* AirbudsTrackTableViewDataSource::CellForIdx(HMUI::TableView* tableView, int idx) {
    if (idx < 0 || static_cast<size_t>(idx) >= rows_.size()) {
        return nullptr;
    }

    const Row& row = rows_.at(static_cast<size_t>(idx));
    if (row.type == RowType::Header) {
        auto tcd = tableView->DequeueReusableCellForIdentifier(AirbudsTrackHeaderTableViewCell::CELL_REUSE_ID);
        AirbudsTrackHeaderTableViewCell* headerCell;
        if (!tcd) {
            auto tableCell = UnityEngine::GameObject::New_ctor("AirbudsTrackHeaderTableViewCell");
            headerCell = tableCell->AddComponent<AirbudsTrackHeaderTableViewCell*>();
            headerCell->set_interactable(false);

            headerCell->set_reuseIdentifier(AirbudsTrackHeaderTableViewCell::CELL_REUSE_ID);
            BSML::parse_and_construct(IncludedAssets::AirbudsTrackHeaderTableViewCell_bsml, headerCell->get_transform(), headerCell);
            headerCell->get_gameObject()->AddComponent<HMUI::Touchable*>();
        } else {
            headerCell = tcd->GetComponent<AirbudsTrackHeaderTableViewCell*>();
        }

        headerCell->setHeader(row.title);
        return headerCell;
    }

    auto tcd = tableView->DequeueReusableCellForIdentifier(AirbudsTrackTableViewCell::CELL_REUSE_ID);
    AirbudsTrackTableViewCell* trackCell;
    if (!tcd) {
        auto tableCell = UnityEngine::GameObject::New_ctor("AirbudsTrackTableViewCell");
        trackCell = tableCell->AddComponent<AirbudsTrackTableViewCell*>();
        trackCell->set_interactable(true);

        trackCell->set_reuseIdentifier(AirbudsTrackTableViewCell::CELL_REUSE_ID);
        BSML::parse_and_construct(IncludedAssets::AirbudsTrackTableViewCell_bsml, trackCell->get_transform(), trackCell);
        trackCell->get_gameObject()->AddComponent<HMUI::Touchable*>();
    } else {
        trackCell = tcd->GetComponent<AirbudsTrackTableViewCell*>();
    }

    trackCell->setTrack(row.track);
    return trackCell;
}

int AirbudsTrackTableViewDataSource::NumberOfCells() {
    return rows_.size();
}

float AirbudsTrackTableViewDataSource::CellSize() {
    return 8.0f;
}

void AirbudsTrackTableViewDataSource::setTracks(std::vector<airbuds::PlaylistTrack> tracks, Grouping grouping) {
    tracks_ = std::move(tracks);
    rows_.clear();

    if (tracks_.empty()) {
        return;
    }

    if (grouping == Grouping::None) {
        rows_.reserve(tracks_.size());
        for (const auto& track : tracks_) {
            rows_.push_back(Row{RowType::Track, "", track});
        }
        return;
    }

    std::string currentKey;

    for (const auto& track : tracks_) {
        std::string key;
        std::string label;
        if (grouping == Grouping::ByDay) {
            const std::string todayKey = getTodayKey();
            if (!getLocalDateKey(track.dateAdded_, key)) {
                key = "unknown";
                label = "Unknown Date";
            } else {
                label = (key == todayKey) ? "Today" : key;
            }
        } else if (grouping == Grouping::ByHour) {
            if (!getLocalHourKey(track.dateAdded_, key, label)) {
                key = "unknown";
                label = "Unknown Time";
            }
        }

        if (currentKey != key) {
            rows_.push_back(Row{RowType::Header, label, {}});
            currentKey = key;
        }
        rows_.push_back(Row{RowType::Track, "", track});
    }
}

void AirbudsTrackTableViewDataSource::clearTracks() {
    tracks_.clear();
    rows_.clear();
}

const airbuds::PlaylistTrack* AirbudsTrackTableViewDataSource::getTrackForRow(int idx) const {
    if (idx < 0 || static_cast<size_t>(idx) >= rows_.size()) {
        return nullptr;
    }
    const Row& row = rows_.at(static_cast<size_t>(idx));
    if (row.type != RowType::Track) {
        return nullptr;
    }
    return &row.track;
}

size_t AirbudsTrackTableViewDataSource::trackCount() const {
    return tracks_.size();
}

const airbuds::PlaylistTrack& AirbudsTrackTableViewDataSource::getTrackAtIndex(size_t idx) const {
    return tracks_.at(idx);
}

int AirbudsTrackTableViewDataSource::getRowIndexForTrackIndex(size_t trackIndex) const {
    if (trackIndex >= tracks_.size()) {
        return -1;
    }
    size_t current = 0;
    for (size_t i = 0; i < rows_.size(); ++i) {
        if (rows_[i].type != RowType::Track) {
            continue;
        }
        if (current == trackIndex) {
            return static_cast<int>(i);
        }
        ++current;
    }
    return -1;
}

int AirbudsTrackTableViewDataSource::getRowIndexForTrack(const airbuds::PlaylistTrack& track) const {
    for (size_t i = 0; i < rows_.size(); ++i) {
        if (rows_[i].type != RowType::Track) {
            continue;
        }
        if (tracksMatch(rows_[i].track, track)) {
            return static_cast<int>(i);
        }
    }
    return -1;
}
