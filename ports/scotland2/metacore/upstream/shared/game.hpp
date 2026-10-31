#pragma once

#include "GlobalNamespace/BeatmapCharacteristicCollection.hpp"
#include "GlobalNamespace/EnvironmentsListModel.hpp"
#include "GlobalNamespace/MainFlowCoordinator.hpp"
#include "GlobalNamespace/MainSystemInit.hpp"
#include "GlobalNamespace/MenuTransitionsHelper.hpp"
#include "GlobalNamespace/PlayerDataModel.hpp"
#include "GlobalNamespace/SongPreviewPlayer.hpp"
#include "UnityEngine/AudioClip.hpp"
#include "UnityEngine/Material.hpp"
#include "Zenject/DiContainer.hpp"
#include "export.h"

namespace MetaCore::Game {
    /// @brief Enables or disables score submission for a mod. Score submission will only be enabled if no mods have it disabled
    /// @param mod The unique id of the mod modifying score submission
    /// @param enable If score submission should be enabled for this mod
    METACORE_EXPORT void SetScoreSubmission(std::string mod, bool enable);
    /// @brief Disables score submission for a mod for the current song only, reenabling on the next GameplaySceneEnded event
    /// @param mod The unique id of the mod modifying score submission
    METACORE_EXPORT void DisableScoreSubmissionOnce(std::string mod);
    /// @brief Gets if score submission is currently disabled
    /// @return If score submission is currently disabled
    METACORE_EXPORT bool IsScoreSubmissionDisabled();
    /// @brief Gets all the mods currently disabling score submission
    /// @return All the mods currently disabling score submission
    METACORE_EXPORT std::set<std::string> const& GetScoreSubmissionDisablers();

    /// @brief Fades the main camera in or out for a mod. If any mod is requesting it, the camera will be faded out (to a black screen)
    /// @param mod The unique id of the mod modifying the camera
    /// @param fadeOut If this mod wants the camera to be faded out
    /// @param fadeDuration The duration for the fade in or out if it changes
    METACORE_EXPORT void SetCameraFadeOut(std::string mod, bool fadeOut, float fadeDuration = -1);
    /// @brief Gets if the main camera is currently faded out (including if the base game does it)
    /// @return If the main camera is currently faded out
    METACORE_EXPORT bool IsCameraFadedOut();

    /// @brief Plays an audio clip with the song preview player, fading out other menu music or song previews
    /// @param audio The audio clip to play (song preview or otherwise)
    /// @param volume An optional volume modifier
    /// @param start The time to start in the audio clip
    /// @param duration The length to play the audio clip for, or the full clip if < 0
    METACORE_EXPORT void PlaySongPreview(UnityEngine::AudioClip* audio, float volume = 0, float start = 0, float duration = -1);

    /// @brief Gets the collection of all beatmap characteristics from the main flow coordinator
    /// @return The BeatmapCharacteristicCollection instance
    METACORE_EXPORT GlobalNamespace::BeatmapCharacteristicCollection* GetCharacteristics();

    /// @brief Gets the collection of all map envirnoments from the main flow coordinator
    /// @return The EnvironmentsListModel instance
    METACORE_EXPORT GlobalNamespace::EnvironmentsListModel* GetEnvironments();

    /// @brief Gets the player data model from the main flow coordinator
    /// @return The PlayerDataModel instance
    METACORE_EXPORT GlobalNamespace::PlayerDataModel* GetPlayerData();

    /// @brief Gets the material used for curved corners on sprites (for example the playlist images)
    /// @return The Material instance
    METACORE_EXPORT UnityEngine::Material* GetCurvedCornersMaterial();

    /// @brief Gets the menu transitions helper from the main flow coordinator
    /// @return The MenuTransitionsHelper instance
    METACORE_EXPORT GlobalNamespace::MenuTransitionsHelper* GetMenuTransitionsHelper();

    /// @brief Gets the main flow coordinator
    /// @return The MainFlowCoordinator instance
    METACORE_EXPORT GlobalNamespace::MainFlowCoordinator* GetMainFlowCoordinator();

    /// @brief Gets the main system init
    /// @return The MainFlowCoordinator instance
    METACORE_EXPORT GlobalNamespace::MainSystemInit* GetMainSystemInit();

    /// @brief Gets the zenject DiContainer from AppInit
    /// @return The DiContainer instance
    METACORE_EXPORT Zenject::DiContainer* GetAppDiContainer();
}
