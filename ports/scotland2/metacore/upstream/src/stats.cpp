#include "stats.hpp"

#include "internals.hpp"
#include "main.hpp"

static inline bool IsLeft(int saber) {
    return saber == MetaCore::Stats::LeftSaber || saber == MetaCore::Stats::BothSabers;
}
static inline bool IsRight(int saber) {
    return saber == MetaCore::Stats::RightSaber || saber == MetaCore::Stats::BothSabers;
}

bool MetaCore::Stats::IsFakeNote(GlobalNamespace::NoteData* data) {
    return data->scoringType == GlobalNamespace::NoteData::ScoringType::NoScore ||
           data->scoringType == GlobalNamespace::NoteData::ScoringType::Ignore;
}

bool MetaCore::Stats::ShouldCountNote(GlobalNamespace::NoteData* data) {
    if (IsFakeNote(data))
        return false;
    return data->gameplayType == GlobalNamespace::NoteData::GameplayType::Normal ||
           data->gameplayType == GlobalNamespace::NoteData::GameplayType::BurstSliderHead;
}

int MetaCore::Stats::GetScore(int saber) {
    int ret = 0;
    if (IsLeft(saber))
        ret += Internals::leftScore;
    if (IsRight(saber))
        ret += Internals::rightScore;
    return ret;
}

int MetaCore::Stats::GetMaxScore(int saber) {
    int ret = 0;
    if (IsLeft(saber))
        ret += Internals::leftMaxScore;
    if (IsRight(saber))
        ret += Internals::rightMaxScore;
    return ret;
}

int MetaCore::Stats::GetSongMaxScore() {
    return Internals::songMaxScore;
}

int MetaCore::Stats::GetCombo(int saber) {
    if (saber == LeftSaber)
        return Internals::leftCombo;
    else if (saber == RightSaber)
        return Internals::rightCombo;
    return Internals::combo;
}

int MetaCore::Stats::GetHighestCombo(int saber) {
    if (saber == LeftSaber)
        return Internals::highestLeftCombo;
    else if (saber == RightSaber)
        return Internals::highestRightCombo;
    return Internals::highestCombo;
}

bool MetaCore::Stats::GetFullCombo(int saber) {
    // ignore walls for individual sabers to match their individual combo trackers
    if (saber == BothSabers && Internals::wallsHit > 0)
        return false;
    if (IsLeft(saber) && Internals::bombsLeftHit + Internals::notesLeftBadCut + Internals::notesLeftMissed > 0)
        return false;
    if (IsRight(saber) && Internals::bombsRightHit + Internals::notesRightBadCut + Internals::notesRightMissed > 0)
        return false;
    return true;
}

int MetaCore::Stats::GetMultiplier() {
    return Internals::multiplier;
}

float MetaCore::Stats::GetMultiplierProgress(bool allLevels) {
    float divisor = allLevels ? 2 + 4 + 8 : GetMultiplier() * 2;
    return GetMultiplierProgressInt(allLevels) / divisor;
}

int MetaCore::Stats::GetMultiplierProgressInt(bool allLevels) {
    if (!allLevels)
        return Internals::multiplierProgress;
    int previous = 0;
    if (Internals::multiplier == 8)
        previous = 2 + 4 + 8;
    else if (Internals::multiplier == 4)
        previous = 2 + 4;
    else if (Internals::multiplier == 2)
        previous = 2;
    return previous + Internals::multiplierProgress;
}

int MetaCore::Stats::GetMaxMultiplier() {
    int notes = GetTotalNotes(BothSabers);
    if (notes < 2)
        return 1;
    if (notes < 2 + 4)
        return 2;
    if (notes < 2 + 4 + 8)
        return 4;
    return 8;
}

float MetaCore::Stats::GetMaxMultiplierProgress(bool allLevels) {
    float divisor = allLevels ? 2 + 4 + 8 : GetMaxMultiplier() * 2;
    return GetMaxMultiplierProgressInt(allLevels) / divisor;
}

int MetaCore::Stats::GetMaxMultiplierProgressInt(bool allLevels) {
    int notes = GetTotalNotes(BothSabers);
    if (allLevels)
        return std::min(notes, 14);
    if (notes >= 2 + 4 + 8)
        return 0;
    if (notes >= 2 + 4)
        return notes - 2 - 4;
    if (notes >= 2)
        return notes - 2;
    return notes;
}

float MetaCore::Stats::GetHealth() {
    return Internals::health;
}

float MetaCore::Stats::GetSongTime() {
    return Internals::songTime;
}

float MetaCore::Stats::GetSongLength() {
    return Internals::songLength;
}

float MetaCore::Stats::GetSongSpeed() {
    return Internals::songSpeed;
}

int MetaCore::Stats::GetTotalNotes(int saber) {
    int ret = 0;
    if (IsLeft(saber))
        ret += Internals::notesLeftCut + Internals::notesLeftBadCut + Internals::notesLeftMissed + Internals::uncountedNotesLeftCut;
    if (IsRight(saber))
        ret += Internals::notesRightCut + Internals::notesRightBadCut + Internals::notesRightMissed + Internals::uncountedNotesRightCut;
    return ret;
}

int MetaCore::Stats::GetNotesCut(int saber, bool includeUncounted) {
    int ret = 0;
    if (IsLeft(saber)) {
        ret += Internals::notesLeftCut;
        if (includeUncounted)
            ret += Internals::uncountedNotesLeftCut;
    }
    if (IsRight(saber)) {
        ret += Internals::notesRightCut;
        if (includeUncounted)
            ret += Internals::uncountedNotesRightCut;
    }
    return ret;
}

int MetaCore::Stats::GetNotesMissed(int saber) {
    int ret = 0;
    if (IsLeft(saber))
        ret += Internals::notesLeftMissed;
    if (IsRight(saber))
        ret += Internals::notesRightMissed;
    return ret;
}

int MetaCore::Stats::GetNotesBadCut(int saber) {
    int ret = 0;
    if (IsLeft(saber))
        ret += Internals::notesLeftBadCut;
    if (IsRight(saber))
        ret += Internals::notesRightBadCut;
    return ret;
}

int MetaCore::Stats::GetBombsHit(int saber) {
    int ret = 0;
    if (IsLeft(saber))
        ret += Internals::bombsLeftHit;
    if (IsRight(saber))
        ret += Internals::bombsRightHit;
    return ret;
}

int MetaCore::Stats::GetWallsHit() {
    return Internals::wallsHit;
}

int MetaCore::Stats::GetSongNotes(int saber) {
    int ret = 0;
    if (IsLeft(saber))
        ret += Internals::songNotesLeft;
    if (IsRight(saber))
        ret += Internals::songNotesRight;
    return ret;
}

int MetaCore::Stats::GetNotesRemaining(int saber) {
    int ret = 0;
    if (IsLeft(saber))
        ret += Internals::remainingNotesLeft;
    if (IsRight(saber))
        ret += Internals::remainingNotesRight;
    return ret;
}

float MetaCore::Stats::GetPreSwing(int saber) {
    int notes = GetNotesCut(saber);
    if (notes == 0)
        return 0;
    int ret = 0;
    if (IsLeft(saber))
        ret += Internals::leftPreSwing;
    if (IsRight(saber))
        ret += Internals::rightPreSwing;
    return ret / (float) notes;
}

float MetaCore::Stats::GetPostSwing(int saber) {
    int notes = GetNotesCut(saber);
    if (notes == 0)
        return 0;
    int ret = 0;
    if (IsLeft(saber))
        ret += Internals::leftPostSwing;
    if (IsRight(saber))
        ret += Internals::rightPostSwing;
    return ret / (float) notes;
}

float MetaCore::Stats::GetAccuracy(int saber) {
    int notes = GetNotesCut(saber);
    if (notes == 0)
        return 0;
    int ret = 0;
    if (IsLeft(saber))
        ret += Internals::leftAccuracy;
    if (IsRight(saber))
        ret += Internals::rightAccuracy;
    return ret / (float) notes;
}

float MetaCore::Stats::GetTimeDependence(int saber) {
    int notes = GetNotesCut(saber);
    if (notes == 0)
        return 0;
    float ret = 0;
    if (IsLeft(saber))
        ret += Internals::leftTimeDependence;
    if (IsRight(saber))
        ret += Internals::rightTimeDependence;
    return ret / notes;
}

float MetaCore::Stats::GetAverageSpeed(int saber) {
    float ret = 0;
    int div = 0;
    if (IsLeft(saber)) {
        for (auto& val : Internals::leftSpeeds)
            ret += val;
        div += Internals::leftSpeeds.size();
    }
    if (IsRight(saber)) {
        for (auto& val : Internals::rightSpeeds)
            ret += val;
        div += Internals::rightSpeeds.size();
    }
    if (div == 0)
        return 0;
    return ret / div;
}

float MetaCore::Stats::GetBestSpeed5Secs(int saber) {
    float ret = 0;
    int back = SLOW_UPDATES_PER_SEC * 5;
    if (IsLeft(saber)) {
        int size = Internals::leftSpeeds.size();
        int start = std::max(0, size - back);
        for (int i = start; i < size; i++)
            ret = std::max(ret, Internals::leftSpeeds[i]);
    }
    if (IsRight(saber)) {
        int size = Internals::rightSpeeds.size();
        int start = std::max(0, size - back);
        for (int i = start; i < size; i++)
            ret = std::max(ret, Internals::rightSpeeds[i]);
    }
    return ret;
}

float MetaCore::Stats::GetLastSecAngle(int saber) {
    float ret = 0;
    int back = SLOW_UPDATES_PER_SEC;
    if (IsLeft(saber)) {
        int size = Internals::leftAngles.size();
        int start = std::max(0, size - back);
        for (int i = start; i < size; i++)
            ret += Internals::leftAngles[i];
    }
    if (IsRight(saber)) {
        int size = Internals::rightAngles.size();
        int start = std::max(0, size - back);
        for (int i = start; i < size; i++)
            ret += Internals::rightAngles[i];
    }
    if (saber == (int) BothSabers)
        ret /= 2;
    return ret;
}

float MetaCore::Stats::GetHighestSecAngle(int saber) {
    float ret = 0;
    if (IsLeft(saber)) {
        for (auto& val : Internals::leftAngles)
            ret = std::max(ret, val);
    }
    if (IsRight(saber)) {
        for (auto& val : Internals::rightAngles)
            ret = std::max(ret, val);
    }
    return ret * SLOW_UPDATES_PER_SEC;
}

float MetaCore::Stats::GetModifierMultiplier(bool positive, bool negative) {
    float ret = 1;
    if (positive)
        ret += Internals::positiveMods;
    if (negative)
        ret += Internals::negativeMods;
    return ret;
}

int MetaCore::Stats::GetBestScore() {
    return Internals::personalBest;
}

int MetaCore::Stats::GetFails() {
    return Internals::fails;
}

int MetaCore::Stats::GetRestarts() {
    return Internals::restarts;
}

int MetaCore::Stats::GetFCScore(int saber) {
    float swingRatio = (GetPreSwing(saber) + GetPostSwing(saber) + GetAccuracy(saber)) / 115;
    int missed = 0;
    int fixed = 0;
    if (IsLeft(saber)) {
        missed += Internals::leftMissedMaxScore;
        fixed += Internals::leftMissedFixedScore;
    }
    if (IsRight(saber)) {
        missed += Internals::rightMissedMaxScore;
        fixed += Internals::rightMissedFixedScore;
    }
    return (missed * swingRatio) + fixed + GetScore(saber);
}
