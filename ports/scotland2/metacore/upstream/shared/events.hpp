#pragma once

#include <functional>
#include <string>

#include "export.h"

namespace MetaCore::Events {
    /// @brief The global ids of all built-in events
    enum Events {
        // After any change to the current or maximum score
        ScoreChanged,
        // After any note is cut, good or bad, and the computation for it is finished
        NoteCut,
        // After any basic or chain note is missed
        NoteMissed,
        // After a bomb is cut
        BombCut,
        // After any wall is entered, even if the player was already inside one
        WallHit,
        // After any change to the combo
        ComboChanged,
        // After any change to the player health / energy
        HealthChanged,
        // Once per frame during a map, after the song time updates
        Update,
        // 4 times per second, unless the framerate is somehow lower
        SlowUpdate,
        // After a new map or difficulty is selected in the menu
        MapSelected,
        // After the current map is deselected without a new one being selected
        MapDeselected,
        // After a new playlist is selected in the menu
        PlaylistSelected,
        // After the current playlist is deselected without a new one being selected
        PlaylistDeselected,
        // After a new map is fully started
        MapStarted,
        // After a map is paused
        MapPaused,
        // After a map is unpaused
        MapUnpaused,
        // After a map is restarted
        MapRestarted,
        // Immediately when a map ends
        MapEnded,
        // After the scene has finished transitioning to gameplay and stats are available
        GameplaySceneStarted,
        // After a map ends or restarts and the scene has transitioned out of gameplay
        GameplaySceneEnded,
        // After the score submission status changes
        ScoreSubmission,
        // Immediately when the game does a soft or internal restart
        SoftRestart,
        EventMax = SoftRestart,
    };

    /// @brief Registers a custom event for future broadcasts
    /// @param mod The unique id of the mod registering the event
    /// @param modEvent The per-mod id of the event being registered
    /// @return The unique global id of the registered event (> EventMax), or -1 on failure
    METACORE_EXPORT int RegisterEvent(std::string mod, int modEvent);

    /// @brief Finds the unique global id of a custom event
    /// @param mod The unique id of the mod that registered the event
    /// @param modEvent The per-mod id of the registered event
    /// @return The unique global id of the event, or -1 if it does not exist
    METACORE_EXPORT int FindEvent(std::string mod, int modEvent);

    /// @brief Registers a callback to an event
    /// @param event The global id of the event
    /// @param callback The function to be called when the event is broadcast
    /// @param once If the callback should only be called once and then removed
    /// @return The id for removal if the event was successfully registered to (>= 0), or -1 on failure
    METACORE_EXPORT int AddCallback(int event, std::function<void()> callback, bool once = false);
    /// @brief Registers a callback to an event
    /// @param mod The unique id of the mod that registered the event
    /// @param modEvent The per-mod id of the event
    /// @param callback The function to be called when the event is broadcast
    /// @param once If the callback should only be called once and then removed
    /// @return The id for removal if the event was successfully registered to (>= 0), or -1 on failure
    METACORE_EXPORT int AddCallback(std::string mod, int modEvent, std::function<void()> callback, bool once = false);
    /// @brief Registers a callback to all events that will be called before event-specific callbacks
    /// @param callback The function to be called with the global id of any broadcast event
    /// @param once If the callback should only be called once and then removed
    /// @return The id for removal (>= 0)
    METACORE_EXPORT int AddCallback(std::function<void(int)> callback, bool once = false);

    /// @brief Removes a previously registered callback
    /// @param id The id returned by a call to AddCallback
    METACORE_EXPORT void RemoveCallback(int id);

    /// @brief Globally broadcasts an event
    /// @param event The global id of the event
    /// @return If the event was successfully broadcast
    METACORE_EXPORT bool Broadcast(int event);
    /// @brief Globally broadcasts an event
    /// @param mod The unique id of the mod that registered the event
    /// @param modEvent The per-mod id of the event
    /// @return If the event was successfully broadcast
    METACORE_EXPORT bool Broadcast(std::string mod, int modEvent);
}

#define CONCAT_WRAPPED(x, y) x##y
#define CONCAT(x, y) CONCAT_WRAPPED(x, y)

#define AUTO_FUNCTION \
static __attribute__((constructor)) void CONCAT(auto_function_dlopen_, __LINE__)()

#define ON_EVENT(...)                                                                     \
static void CONCAT(auto_function_event_, __LINE__)();                                     \
AUTO_FUNCTION {                                                                           \
    ::MetaCore::Events::AddCallback(__VA_ARGS__, CONCAT(auto_function_event_, __LINE__)); \
}                                                                                         \
static void CONCAT(auto_function_event_, __LINE__)()
