#pragma once

#include "GlobalNamespace/NoteData.hpp"
#include "export.h"

namespace MetaCore::Stats {
    /// @brief Checks if a note is a fake note, such as from noodle extensions
    /// @param data The note data
    /// @return If the note is fake
    METACORE_EXPORT bool IsFakeNote(GlobalNamespace::NoteData* data);
    /// @brief Checks if a note should be counted in calculations involving swing parts or note count
    /// @param data The note data
    /// @return If the note should be counted
    METACORE_EXPORT bool ShouldCountNote(GlobalNamespace::NoteData* data);

    constexpr int LeftSaber = 0;
    constexpr int RightSaber = 1;
    constexpr int BothSabers = 2;

    // no per-function documentation here, sorry

    METACORE_EXPORT int GetScore(int saber);
    METACORE_EXPORT int GetMaxScore(int saber);
    METACORE_EXPORT int GetSongMaxScore();
    METACORE_EXPORT int GetCombo(int saber);
    METACORE_EXPORT int GetHighestCombo(int saber);
    METACORE_EXPORT bool GetFullCombo(int saber);
    METACORE_EXPORT int GetMultiplier();
    METACORE_EXPORT float GetMultiplierProgress(bool allLevels);
    METACORE_EXPORT int GetMultiplierProgressInt(bool allLevels);
    METACORE_EXPORT int GetMaxMultiplier();
    METACORE_EXPORT float GetMaxMultiplierProgress(bool allLevels);
    METACORE_EXPORT int GetMaxMultiplierProgressInt(bool allLevels);
    METACORE_EXPORT float GetHealth();
    METACORE_EXPORT float GetSongTime();
    METACORE_EXPORT float GetSongLength();
    METACORE_EXPORT float GetSongSpeed();
    METACORE_EXPORT int GetTotalNotes(int saber);
    METACORE_EXPORT int GetNotesCut(int saber, bool includeUncounted = false);
    METACORE_EXPORT int GetNotesMissed(int saber);
    METACORE_EXPORT int GetNotesBadCut(int saber);
    METACORE_EXPORT int GetBombsHit(int saber);
    METACORE_EXPORT int GetWallsHit();
    METACORE_EXPORT int GetSongNotes(int saber);
    METACORE_EXPORT int GetNotesRemaining(int saber);
    METACORE_EXPORT float GetPreSwing(int saber);
    METACORE_EXPORT float GetPostSwing(int saber);
    METACORE_EXPORT float GetAccuracy(int saber);
    METACORE_EXPORT float GetTimeDependence(int saber);
    METACORE_EXPORT float GetAverageSpeed(int saber);
    METACORE_EXPORT float GetBestSpeed5Secs(int saber);
    METACORE_EXPORT float GetLastSecAngle(int saber);
    METACORE_EXPORT float GetHighestSecAngle(int saber);
    METACORE_EXPORT float GetModifierMultiplier(bool positive, bool negative);
    METACORE_EXPORT int GetBestScore();
    METACORE_EXPORT int GetFails();
    METACORE_EXPORT int GetRestarts();
    METACORE_EXPORT int GetFCScore(int saber);
}
