#pragma once

#include "GlobalNamespace/BeatmapLevel.hpp"
#include "SimpleSafePtr.hpp"
#include "Types/BPList.hpp"
#include "UnityEngine/Sprite.hpp"
#include "songcore/shared/SongLoader/CustomBeatmapLevelsRepository.hpp"
#include "songcore/shared/SongLoader/CustomLevelPack.hpp"

namespace PlaylistCore {

    /// @brief A struct that wraps the data for a playlist
    struct Playlist {
        BPList playlistJSON;
        SimpleSafePtr<SongCore::SongLoader::CustomLevelPack> playlistCS;
        std::string name;
        std::string path;
        int imageIndex = -1;
        void Save();
    };

    /// @brief Whether the playlists have loaded at least once yet
    extern bool hasLoaded;

    /// @brief Gets the default playlist cover image
    /// @return The sprite with the default cover image
    UnityEngine::Sprite* GetDefaultCoverImage();

    /// @brief Gets the cover image for a playlist
    /// @param playlist The playlist to retrieve the cover image from
    /// @return The sprite with the playlist's cover image
    UnityEngine::Sprite* GetCoverImage(Playlist* playlist);

    /// @brief Deletes a loaded image from loaded images and its file
    /// @param index The index of the image in the loaded images array
    void DeleteLoadedImage(int index);

    /// @brief Saves an image to the covers directory and loads it
    /// @param texture The texture of the image to add
    /// @param fileName The file name to write to in the image directory, may be modified
    /// @return The index of the new image in the loaded images array
    int AddCoverImage(UnityEngine::Texture2D* texture, std::string const& fileName);

    /// @brief Loads all unloaded cover images from the cover folder
    void LoadCoverImages();

    /// @brief Gets all images that are loaded
    /// @return A vector of all loaded images in order
    std::vector<UnityEngine::Sprite*> const& GetLoadedImages();

    /// @brief Unloads all loaded images
    void ClearLoadedImages();

    /// @brief Loads all playlists from the playlists folder, adding them to the collection
    /// @param customBeatmapLevelsRepository The level pack collection that the loaded playlists will be added to
    /// @param fullReload Whether to reload the contents of already loaded playlists
    void LoadPlaylists(SongCore::SongLoader::CustomBeatmapLevelsRepository* customBeatmapLevelsRepository, bool fullReload = false);

    /// @brief Gets all playlists that are loaded
    /// @return A vector of all loaded playlists in order
    std::vector<Playlist*> GetLoadedPlaylists();

    /// @brief Gets a playlist based on the string of its path, which is different from its ID
    /// @return The playlist with that path
    Playlist* GetPlaylist(std::string const& path);

    /// @brief Gets a playlist based on its ingame ID, which is its path plus the custom_levelPack_ prefix
    /// @return The playlist with that ID
    Playlist* GetPlaylistWithPrefix(std::string const& id);

    /// @brief Gets the index of a playlist in the order config
    /// @param addIfMissing Whether to add the playlist to the end of the order config if not contained
    /// @return The index in the full order config, or -1 if it was not contained
    int GetPlaylistIndex(std::string const& path, bool addIfMissing = true);

    /// @brief Returns whether or not a playlist is currently visible in the game's selection menu, based on all playlist filters
    /// @return If the playlist is currently visible
    bool IsPlaylistShown(std::string const& path);

    /// @brief Adds a function to filter out playlists from being shown.
    /// The function will receive the path to each custom playlist, and the string literal "Defaults" for the Custom and Custom WIP playlists
    /// @param mod The ModInfo of the mod registering the filter
    /// @param function The filter function - takes a playlist path and returns whether it should be shown
    void AddPlaylistFilter(modloader::ModInfo mod, std::function<bool(std::string const& path)> function);

    /// @brief Removes all of the playlist filter functions added by a mod
    /// @param mod The ModInfo of the mod
    void RemovePlaylistFilters(modloader::ModInfo mod);

    /// @brief Creates a new playlist file
    /// @param title The name of the playlist to be created
    /// @param author The author of the playlist to be created
    /// @param coverImage The cover image for the playlist - does not have to be a loaded image
    /// @param reloadPlaylists Whether to reload playlists (not fully) after creating
    /// @return The path to the created playlist and the playlist itself if loaded, otherwise nullptr
    std::pair<std::string, Playlist*>
    AddPlaylist(std::string const& title, std::string const& author, UnityEngine::Sprite* coverImage = nullptr, bool reloadPlaylists = true);

    /// @brief Creates a new playlist file
    /// @param playlist The BPList object to create
    /// @param reloadPlaylists Whether to reload playlists (not fully) after creating
    /// @return The path to the created playlist and the playlist itself if loaded, otherwise nullptr
    std::pair<std::string, Playlist*> AddPlaylist(BPList playlist, bool reloadPlaylists = true);

    /// @brief Moves a playlist in the order config - does not reload playlists
    /// @param playlist The playlist to be moved
    /// @param index The index to move the playlist to
    void MovePlaylist(Playlist* playlist, int index);

    /// @brief Renames a playlist - does not reload playlists, but updates level pack name
    /// @param playlist The playlist to rename
    /// @param title The new name for the playlist
    void RenamePlaylist(Playlist* playlist, std::string const& title);

    /// @brief Changes the cover of a playlist to the cover at the index - does not reload playlists, but updates level pack sprites
    /// @param playlist The playlist to change the cover of
    /// @param index The index of the new cover image in the loaded images array
    void ChangePlaylistCover(Playlist* playlist, int index);

    /// @brief Deletes a playlist and its file - does not reload playlists
    /// @param playlist The playlist to delete
    void DeletePlaylist(Playlist* playlist);

    /// @brief Reloads playlists
    /// @param fullReload Whether to reload the contents of already loaded playlists
    void ReloadPlaylists(bool fullReload = false);

    /// @brief Makes a single playlist be fully reloaded on the next reload
    /// @param playlist The playlist to be reloaded
    void MarkPlaylistForReload(Playlist* playlist);

    /// @brief Checks whether or not a playlist is missing songs
    /// @param playlist The playlist to check for missing songs
    /// @return The number of songs missing from the playlist
    int PlaylistHasMissingSongs(Playlist* playlist);

    /// @brief Attempts to download all missing songs from a playlist
    /// @param playlist The playlist to download missing songs for
    /// @param onFinished A main thread callback whenever all songs have downloaded
    /// @param onProgress A main thread callback called immediately and whenever a song has downloaded - params (total, numFinished)
    void DownloadMissingSongsFromPlaylist(Playlist* playlist, std::function<void()> onFinished, std::function<void(int, int)> onProgress);

    /// @brief Removes songs that are supposed to be in a playlist but not owned from the playlist - only updates playlist JSON
    /// @param playlist The playlist to remove missing songs from
    void RemoveMissingSongsFromPlaylist(Playlist* playlist);

    /// @brief Adds a song to a playlist - does not reload playlists, but updates the loaded levels array
    /// @param playlist The playlist to add the song to
    /// @param level The song to add to the playlist
    void AddSongToPlaylist(Playlist* playlist, GlobalNamespace::BeatmapLevel* level);

    /// @brief Removes a song from a playlist - does not reload playlists, but updates the loaded levels array
    /// @param playlist The playlist to remove the song from
    /// @param level The song to remove from the playlist
    void RemoveSongFromPlaylist(Playlist* playlist, GlobalNamespace::BeatmapLevel* level);

    /// @brief Removes a song from all loaded playlists - does not reload playlists, but updates the loaded levels array
    /// @param level The song to remove from all playlists
    void RemoveSongFromAllPlaylists(GlobalNamespace::BeatmapLevel* level);

    /// @brief Changes the index of a song inside a playlist - does not reload playlists, but updates the loaded levels array
    /// @param playlist The playlist containing the song to be reordered
    /// @param index The new index for the song to be at, based on the loaded levels array if not all are loaded
    void SetSongIndex(Playlist* playlist, GlobalNamespace::BeatmapLevel* level, int index);
}
