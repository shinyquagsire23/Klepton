#include "PlaylistCore.hpp"

#include "GlobalNamespace/BeatmapLevel.hpp"
#include "GlobalNamespace/CustomLevelLoader.hpp"
#include "Main.hpp"
#include "ResettableStaticPtr.hpp"
#include "Settings.hpp"
#include "SpriteCache.hpp"
#include "System/Convert.hpp"
#include "System/IO/File.hpp"
#include "Types/BPList.hpp"
#include "Types/Config.hpp"
#include "UnityEngine/ImageConversion.hpp"
#include "UnityEngine/Resources.hpp"
#include "UnityEngine/SpriteMeshType.hpp"
#include "UnityEngine/Texture2D.hpp"
#include "UnityEngine/TextureFormat.hpp"
#include "Utils.hpp"
#include "assets.hpp"
#include "beatsaber-hook/shared/utils/il2cpp-utils.hpp"
#include "beatsaverplusplus/shared/BeatSaver.hpp"
#include "bsml/shared/BSML/MainThreadScheduler.hpp"
#include "metacore/shared/songs.hpp"
#include "metacore/shared/strings.hpp"
#include "metacore/shared/unity.hpp"
#include "songcore/shared/SongCore.hpp"

using namespace SongCore;
using namespace SongCore::SongLoader;
using namespace PlaylistCore::Utils;
using namespace GlobalNamespace;

namespace PlaylistCore {

    std::map<std::string, Playlist*> path_playlists;
    std::unordered_map<UnityEngine::Sprite*, std::string> image_paths;

    // array of all loaded images
    std::vector<UnityEngine::Sprite*> loadedImages;

    bool hasLoaded = false;
    // all unusable playlists
    std::unordered_set<std::string> staticPackIDs{};
    // playlists that need to be reloaded on the next reload
    std::unordered_set<Playlist*> needsReloadPlaylists{};
    // functions that filter out playlists from being shown
    std::vector<std::pair<modloader::ModInfo, std::function<bool(std::string const& path)>>> playlistFilters;

    void Playlist::Save() {
        if (!WriteToFile(path, playlistJSON))
            LOG_ERROR("Error saving playlist! Path: {}", path);
    }

    UnityEngine::Sprite* GetDefaultCoverImage() {
        if (auto ret = HasCachedSprite("default"))
            return ret;
        auto ret = BSML::Lite::ArrayToSprite(IncludedAssets::LevelPack_png);
        CacheSprite(ret, "default");
        return ret;
    }

    UnityEngine::Sprite* GetCoverImage(Playlist* playlist) {
        // changes to playlist cover should change index as well
        if (playlist->imageIndex >= 0)
            return loadedImages[playlist->imageIndex];
        // index is -1 with unloaded or default cover image
        auto& json = playlist->playlistJSON;
        if (json.ImageString.has_value()) {
            std::string_view imageBase64 = json.ImageString.value();
            // trim "data:image/png;base64,"-like metadata
            static std::string searchString = "base64,";
            // only search first ~20 characters
            auto searchIndex = imageBase64.substr(0, 40).find(searchString);
            if (searchIndex != std::string::npos)
                imageBase64 = imageBase64.substr(searchIndex + searchString.length());
            // return loaded image if existing
            if (auto sprite = HasCachedSprite(imageBase64)) {
                LOG_INFO("Returning loaded image");
                int index = std::find(loadedImages.begin(), loadedImages.end(), sprite) - loadedImages.begin();
                playlist->imageIndex = index;
                return sprite;
            }
            // check image type
            std::string imgType = GetBase64ImageType(imageBase64);
            if (imgType != ".png" && imgType != ".jpg") {
                LOG_ERROR("Unsupported image type {}", imgType);
                return GetDefaultCoverImage();
            }
            // get and write texture
            auto texture = UnityEngine::Texture2D::New_ctor(0, 0, UnityEngine::TextureFormat::RGBA32, false, false);
            LOG_INFO("Loading image of playlist {}", playlist->name);
            try {
                UnityEngine::ImageConversion::LoadImage(texture, System::Convert::FromBase64String(imageBase64));  // copy
            } catch (std::exception const& exc) {
                LOG_DEBUG("Error loading image: {}", exc.what());
                return GetDefaultCoverImage();
            }
            // process texture size and png string and check hash for changes
            auto newImageBase64 = ProcessImage(texture, true);  // probably most expensive idk
            // write to playlist if changed
            if (newImageBase64 != imageBase64) {
                json.ImageString = newImageBase64;  // copy
                playlist->Save();
            }
            if (auto sprite = HasCachedSprite(newImageBase64)) {
                LOG_INFO("Returning loaded image");
                int index = std::find(loadedImages.begin(), loadedImages.end(), sprite) - loadedImages.begin();
                playlist->imageIndex = index;
                return sprite;
            }
            // save image as file with playlist file name and return
            std::string playlistPathName = std::filesystem::path(playlist->path).stem();
            std::string imgPath = GetCoversPath() + "/" + playlistPathName + ".png";
            LOG_INFO("Writing image from playlist to {}", imgPath);
            MetaCore::Engine::WriteTexture(texture, imgPath);
            auto sprite = UnityEngine::Sprite::Create(
                texture,
                UnityEngine::Rect(0, 0, texture->get_width(), texture->get_height()),
                {0.5, 0.5},
                1024,
                1,
                UnityEngine::SpriteMeshType::FullRect,
                {0, 0, 0, 0},
                false
            );
            CacheSprite(sprite, std::move(newImageBase64));
            image_paths.insert({sprite, imgPath});
            playlist->imageIndex = loadedImages.size();
            loadedImages.emplace_back(sprite);
            return sprite;
        }
        playlist->imageIndex = -1;
        return GetDefaultCoverImage();
    }

    void DeleteLoadedImage(int index) {
        auto sprite = loadedImages[index];
        // get path
        auto pathItr = image_paths.find(loadedImages[index]);
        if (pathItr == image_paths.end())
            return;
        // update image indices of playlists
        for (auto& playlist : GetLoadedPlaylists()) {
            if (playlist->imageIndex == index)
                playlist->imageIndex = -1;
            if (playlist->imageIndex > index)
                playlist->imageIndex--;
        }
        // remove from path map and delete file
        std::filesystem::remove(pathItr->second);
        image_paths.erase(sprite);
        // remove from loaded images
        loadedImages.erase(loadedImages.begin() + index);
        RemoveCachedSprite(sprite);
    }

    int AddCoverImage(UnityEngine::Texture2D* texture, std::string const& fileName) {
        // downscale and convert to png
        std::string imageString = ProcessImage(texture, true);
        // return existing image if cached
        if (auto sprite = HasCachedSprite(imageString)) {
            int index = std::find(loadedImages.begin(), loadedImages.end(), sprite) - loadedImages.begin();
            return index;
        }
        // remove any directories
        std::string path;
        auto dirPos = fileName.find_last_of("/");
        if (dirPos != std::string::npos)
            path = fileName.substr(dirPos);
        else
            path = fileName;
        // sanitize file name
        path = MetaCore::Strings::SanitizedPath(path);
        if (!path.ends_with(".png"))
            path += ".png";
        while (!UniqueFileName(path, GetCoversPath()))
            path = "_" + path;
        // put in covers path
        path = GetCoversPath() + "/" + path;
        LOG_INFO("Saving image {}", path);
        // save and load
        MetaCore::Engine::WriteTexture(texture, path);
        auto sprite = UnityEngine::Sprite::Create(
            texture,
            UnityEngine::Rect(0, 0, texture->get_width(), texture->get_height()),
            {0.5, 0.5},
            1024,
            1,
            UnityEngine::SpriteMeshType::FullRect,
            {0, 0, 0, 0},
            false
        );
        CacheSprite(sprite, std::move(imageString));
        image_paths.insert({sprite, path});
        loadedImages.emplace_back(sprite);
        return loadedImages.size() - 1;
    }

    void LoadCoverImages() {
        // ensure path exists
        auto imagePath = GetCoversPath();
        if (!std::filesystem::is_directory(imagePath))
            return;
        std::vector<std::filesystem::path> paths;
        // iterate through all image files
        for (auto const& file : std::filesystem::directory_iterator(imagePath)) {
            if (!file.is_directory())
                paths.emplace_back(file.path());
        }
        for (auto path : paths) {
            std::string extension = path.extension().string();
            LOWER(extension);
            // check file extension
            if (extension == ".jpg") {
                auto newPath = path.parent_path() / (path.stem().string() + ".png");
                std::filesystem::rename(path, newPath);
                path = newPath;
            } else if (extension != ".png") {
                LOG_ERROR("Incompatible file extension: {}", extension);
                continue;
            }
            // check hash of base image before converting to sprite and to png
            auto bytes = System::IO::File::ReadAllBytes(path.string());
            std::string imageString = System::Convert::ToBase64String(bytes);
            if (HasCachedSprite(imageString)) {
                LOG_INFO("Skipping loading image {}", path.string());
                continue;
            }
            // sanatize hash by converting to png
            auto texture = UnityEngine::Texture2D::New_ctor(0, 0, UnityEngine::TextureFormat::RGBA32, false, false);
            try {
                UnityEngine::ImageConversion::LoadImage(texture, bytes);
            } catch (std::exception const& exc) {
                LOG_DEBUG("Error loading image: {}", exc.what());
                continue;
            }
            std::string newImageString = ProcessImage(texture, true);
            if (newImageString != imageString)
                MetaCore::Engine::WriteTexture(texture, path.string());
            // check hash with loaded images
            if (HasCachedSprite(newImageString)) {
                LOG_INFO("Skipping loading image {}", path.string());
                continue;
            }
            LOG_INFO("Loading image {}", path.string());
            auto sprite = UnityEngine::Sprite::Create(
                texture,
                UnityEngine::Rect(0, 0, texture->get_width(), texture->get_height()),
                {0.5, 0.5},
                1024,
                1,
                UnityEngine::SpriteMeshType::FullRect,
                {0, 0, 0, 0},
                false
            );
            CacheSprite(sprite, std::move(newImageString));
            image_paths.insert({sprite, path});
            loadedImages.emplace_back(sprite);
        }
    }

    std::vector<UnityEngine::Sprite*> const& GetLoadedImages() {
        return loadedImages;
    }

    void ClearLoadedImages() {
        loadedImages.clear();
        image_paths.clear();
        ClearCachedSprites();
    }

    void LoadPlaylists(CustomBeatmapLevelsRepository* customBeatmapLevelsRepository, bool fullReload) {
        LOG_INFO("Loading playlists");
        RemoveAllBMBFSuffixes();
        // can be laggy depending on the number of images, but generally only loads a lot on launch when the screen is black anyway
        LoadCoverImages();
        // clear playlists if requested
        if (fullReload) {
            for (auto& pair : path_playlists)
                MarkPlaylistForReload(pair.second);
        }
        // ensure path exists
        auto path = GetPlaylistsPath();
        if (!std::filesystem::exists(path) || !std::filesystem::is_directory(path))
            return;
        // clear out old playlists if showDefaults is off
        if (!IsPlaylistShown("Defaults")) {
            LOG_INFO("Removing default playlists from being shown");
            customBeatmapLevelsRepository->RemoveLevelPack(API::Loading::GetCustomLevelPack());
            customBeatmapLevelsRepository->RemoveLevelPack(API::Loading::GetCustomWIPLevelPack());
        }
        // create set of playlists that aren't found when loading
        std::unordered_set<std::string> removedPaths{};
        auto orderVec = getConfig().Order.GetValue();
        for (auto& path : orderVec)
            removedPaths.insert(path);
        // create array for playlists
        std::vector<CustomLevelPack*> sortedPlaylists(orderVec.size());
        // iterate through all playlist files
        for (auto const& entry : std::filesystem::directory_iterator(path)) {
            if (!entry.is_directory()) {
                Playlist* playlist = nullptr;
                // check if playlist has been loaded already
                auto path = entry.path().string();
                auto path_iter = path_playlists.find(path);
                if (path_iter != path_playlists.end())
                    playlist = path_iter->second;
                // load from cache without reload
                if (playlist && !needsReloadPlaylists.contains(playlist)) {
                    LOG_INFO("Loading playlist file {} from cache", path);
                    // check if playlist should be added
                    // check if playlist needs to be reloaded
                    if (IsPlaylistShown(playlist->path)) {
                        int packPosition = GetPlaylistIndex(playlist->path);
                        // add if new (idk how)
                        if (packPosition < 0)
                            sortedPlaylists.emplace_back(playlist->playlistCS);
                        else
                            sortedPlaylists[packPosition] = playlist->playlistCS;
                    }
                } else {
                    LOG_INFO("Loading playlist file {}", path);
                    // only create a new playlist if one doesn't exist
                    // if one does, its contents will simply be overwritten with the reloaded data
                    if (!playlist)
                        playlist = new Playlist();
                    else {
                        needsReloadPlaylists.erase(playlist);
                        // clear cached data in playlist object
                        playlist->imageIndex = -1;
                    }
                    // get playlist object from file
                    bool success = false;
                    try {
                        ReadFromFile(path, playlist->playlistJSON);
                        success = true;
                    } catch (std::exception const& err) {
                        LOG_ERROR("Error loading playlist {}: {}", path, err.what());
                        success = false;
                    }
                    if (success) {
                        playlist->name = playlist->playlistJSON.PlaylistTitle;
                        playlist->path = path;
                        path_playlists.insert({playlist->path, playlist});
                        // create playlist object
                        std::string id = "custom_levelPack_" + playlist->path;
                        CustomLevelPack* beatmapLevelPack = CustomLevelPack::New(id, playlist->name, GetCoverImage(playlist));
                        playlist->playlistCS = beatmapLevelPack;
                        // clear out duplicate songs
                        auto& songs = playlist->playlistJSON.Songs;
                        std::unordered_set<std::string> levelIds{};
                        // add all songs to the playlist object
                        std::vector<BeatmapLevel*> foundSongs;
                        for (auto itr = songs.begin(); itr != songs.end();) {
                            if (levelIds.contains(itr->LevelID))
                                itr = songs.erase(itr);
                            else {
                                levelIds.insert(itr->LevelID);
                                if (auto search = MetaCore::Songs::FindLevel(itr->LevelID))
                                    foundSongs.emplace_back(search);
                                else if (itr->Hash) {
                                    LOG_INFO("level id {} not found, attempting to use hash", itr->LevelID);
                                    if (auto search = MetaCore::Songs::FindLevel(*itr->Hash)) {
                                        // fix levelid and hash
                                        itr->LevelID = *itr->Hash;
                                        itr->Hash = MetaCore::Songs::GetHash(itr->LevelID);
                                        if (itr->Hash->empty())
                                            itr->Hash = std::nullopt;
                                        foundSongs.emplace_back(search);
                                    } else
                                        LOG_ERROR("level id {} not found", *itr->Hash);
                                } else
                                    LOG_ERROR("level id {} not found", itr->LevelID);
                                itr++;
                            }
                        }
                        // save removed duplicates
                        playlist->Save();
                        beatmapLevelPack->SetLevels(foundSongs);
                        // add the playlist to the sorted array
                        if (IsPlaylistShown(playlist->path)) {
                            int packPosition = GetPlaylistIndex(playlist->path);
                            // add if new
                            if (packPosition < 0)
                                sortedPlaylists.emplace_back(beatmapLevelPack);
                            else
                                sortedPlaylists[packPosition] = beatmapLevelPack;
                        }
                    } else {
                        delete playlist;
                        playlist = nullptr;
                    }
                }
                // keep path in order config if loaded
                if (playlist && removedPaths.contains(path))
                    removedPaths.erase(path);
            }
        }
        // add playlists to game in sorted order
        for (auto beatmapLevelPack : sortedPlaylists) {
            if (beatmapLevelPack)
                customBeatmapLevelsRepository->AddLevelPack(beatmapLevelPack);
        }
        // remove paths in order config that were not loaded
        for (auto& path : removedPaths) {
            for (auto iter = orderVec.begin(); iter != orderVec.end(); iter++) {
                if (*iter == path) {
                    orderVec.erase(iter);
                    iter--;
                }
            }
            // delete them if they were still loaded as well
            auto pathItr = path_playlists.find(path);
            if (pathItr != path_playlists.end()) {
                delete pathItr->second;
                path_playlists.erase(pathItr);
            }
        }
        getConfig().Order.SetValue(orderVec);
        hasLoaded = true;
        LOG_INFO("Playlists loaded");
    }

    std::vector<Playlist*> GetLoadedPlaylists() {
        // create return vector with base size
        std::vector<Playlist*> playlistArray(getConfig().Order.GetValue().size());
        for (auto& pair : path_playlists) {
            auto& playlist = pair.second;
            int idx = GetPlaylistIndex(playlist->path);
            if (idx >= 0)
                playlistArray[idx] = playlist;
            else
                playlistArray.push_back(playlist);
        }
        // remove empty slots
        for (auto itr = playlistArray.begin(); itr != playlistArray.end();) {
            if (*itr == nullptr)
                itr = playlistArray.erase(itr);
            else
                itr++;
        }
        return playlistArray;
    }

    Playlist* GetPlaylist(std::string const& path) {
        auto iter = path_playlists.find(path);
        if (iter == path_playlists.end())
            return nullptr;
        return iter->second;
    }

    Playlist* GetPlaylistWithPrefix(std::string const& id) {
        static int const prefixLength = std::string(CustomLevelPackPrefixID).length();
        if (id.starts_with(CustomLevelPackPrefixID))
            return GetPlaylist(id.substr(prefixLength));
        return nullptr;
    }

    int GetPlaylistIndex(std::string const& path, bool add) {
        auto orderVec = getConfig().Order.GetValue();
        // find index of playlist title in config
        for (int i = 0; i < orderVec.size(); i++) {
            if (orderVec[i] == path)
                return i;
        }
        // add to end of config if not found
        if (add) {
            orderVec.push_back(path);
            getConfig().Order.SetValue(orderVec);
        }
        return -1;
    }

    bool IsPlaylistShown(std::string const& path) {
        bool shown = true;
        for (auto& pair : playlistFilters)
            shown = shown && pair.second(path);
        return shown;
    }

    void AddPlaylistFilter(modloader::ModInfo mod, std::function<bool(std::string const& path)> func) {
        playlistFilters.emplace_back(std::make_pair(mod, func));
    }

    void RemovePlaylistFilters(modloader::ModInfo mod) {
        for (auto itr = playlistFilters.begin(); itr != playlistFilters.end();) {
            if (itr->first.id == mod.id && itr->first.version == mod.version)
                itr = playlistFilters.erase(itr);
            else
                itr++;
        }
    }

    std::pair<std::string, Playlist*>
    AddPlaylist(std::string const& title, std::string const& author, UnityEngine::Sprite* coverImage, bool reloadPlaylists) {
        // create playlist with info
        auto newPlaylist = BPList();
        newPlaylist.PlaylistTitle = title;
        if (author != "")
            newPlaylist.PlaylistAuthor = author;
        if (coverImage) {
            auto bytes = UnityEngine::ImageConversion::EncodeToPNG(coverImage->get_texture());
            newPlaylist.ImageString = System::Convert::ToBase64String(bytes);
        }
        // add bplist
        return AddPlaylist(newPlaylist, reloadPlaylists);
    }

    std::pair<std::string, Playlist*> AddPlaylist(BPList playlist, bool reloadPlaylists) {
        // save playlist
        std::string path = GetNewPlaylistPath(playlist.PlaylistTitle);
        WriteToFile(path, playlist);
        Playlist* ret = nullptr;
        if (reloadPlaylists) {
            ReloadPlaylists();
            ret = GetPlaylist(path);
        }
        return {path, ret};
    }

    void MovePlaylist(Playlist* playlist, int index) {
        int originalIndex = GetPlaylistIndex(playlist->path);
        if (originalIndex < 0) {
            LOG_ERROR("Attempting to move unloaded playlist");
            return;
        }
        auto orderVec = getConfig().Order.GetValue();
        orderVec.erase(orderVec.begin() + originalIndex);
        orderVec.insert(orderVec.begin() + index, playlist->path);
        getConfig().Order.SetValue(orderVec);
    }

    void RenamePlaylist(Playlist* playlist, std::string const& title) {
        // edit variables
        playlist->name = title;
        playlist->playlistJSON.PlaylistTitle = title;
        // rename playlist ingame
        auto& levelPack = playlist->playlistCS;
        if (levelPack) {
            levelPack->packName = title;
            levelPack->shortPackName = title;
        }
        // save changes
        playlist->Save();
    }

    void ChangePlaylistCover(Playlist* playlist, int index) {
        UnityEngine::Sprite* newCover = nullptr;
        // update json image string
        auto& json = playlist->playlistJSON;
        if (index < 0) {
            newCover = GetDefaultCoverImage();
            // don't save string for default cover
            json.ImageString = std::nullopt;
        } else {
            newCover = GetLoadedImages()[index];
            // save image base 64
            auto bytes = UnityEngine::ImageConversion::EncodeToPNG(newCover->get_texture());
            json.ImageString = System::Convert::ToBase64String(bytes);
        }
        playlist->imageIndex = index;
        // change cover ingame
        auto& levelPack = playlist->playlistCS;
        if (levelPack) {
            levelPack->coverImage = newCover;
            levelPack->smallCoverImage = newCover;
        }
        playlist->Save();
    }

    void DeletePlaylist(Playlist* playlist) {
        // remove from map
        auto path_iter = path_playlists.find(playlist->path);
        if (path_iter == path_playlists.end()) {
            LOG_ERROR("Could not find playlist by path");
            return;
        }
        path_playlists.erase(path_iter);
        // delete file
        std::filesystem::remove(playlist->path);
        // remove name from order config
        int orderIndex = GetPlaylistIndex(playlist->path);
        auto orderVec = getConfig().Order.GetValue();
        if (orderIndex >= 0)
            orderVec.erase(orderVec.begin() + orderIndex);
        else
            orderVec.erase(orderVec.end() - 1);
        getConfig().Order.SetValue(orderVec);
        // delete playlist object
        delete playlist;
    }

    void ReloadPlaylists(bool fullReload) {
        if (!hasLoaded)
            return;
        // handle full reload here since songloader's full refesh isn't carried through
        // also, we don't want to always full reload songs at the same time as playlists
        if (fullReload) {
            for (auto& pair : path_playlists)
                MarkPlaylistForReload(pair.second);
        }
        API::Loading::RefreshLevelPacks();
    }

    void MarkPlaylistForReload(Playlist* playlist) {
        needsReloadPlaylists.insert(playlist);
    }

    int PlaylistHasMissingSongs(Playlist* playlist) {
        int songsMissing = 0;
        for (auto& song : playlist->playlistJSON.Songs) {
            std::string& id = song.LevelID;
            bool hasSong = false;
            // search in songs in playlist instead of all songs
            // we need to treat the list as an array because it is initialized as an array elsewhere
            ArrayW<BeatmapLevel*> levelList(playlist->playlistCS->_beatmapLevels);
            for (int i = 0; i < levelList.size(); i++) {
                if (MetaCore::Strings::IEquals(id, levelList[i]->levelID)) {
                    hasSong = true;
                    break;
                }
            }
            if (!hasSong)
                songsMissing += 1;
        }
        return songsMissing;
    }

    struct GetBeatmapsForDownloadRequest : WebUtils::GenericRequest<BeatSaver::API::BeatmapMapResponse> {
        GetBeatmapsForDownloadRequest(std::span<std::string> hashes) :
            GenericRequest(BeatSaver::API::GetBeatmapsByHashesURLOptions(hashes)),
            hashes(hashes.begin(), hashes.end()) {}

        BeatSaver::API::BeatmapDownloadInfo GetDownload(std::string const& hash) {
            std::optional<BeatSaver::Models::Beatmap> foundMap = std::nullopt;
            for (auto& [key, map] : *targetResponse.responseData) {
                for (auto& version : map.Versions) {
                    if (MetaCore::Strings::IEquals(version.Hash, hash))
                        return {map, version};
                }
                if (MetaCore::Strings::IEquals(key, hash))
                    foundMap = map;
            }
            if (foundMap)
                logger.warn("failed to find version of beatmap with hash {}, attempting alternative download", hash);
            else
                logger.warn("failed to find beatmap with hash {}, attempting download anyway", hash);
            std::string key = foundMap ? foundMap->Id : hash;
            std::string folder = foundMap ? foundMap->CreateFolderName() : hash;
            return {key, fmt::format(BEATSAVER_CDN_URL "/{}.zip", hash), folder};
        }
        std::vector<BeatSaver::API::BeatmapDownloadInfo> GetDownloads() {
            std::vector<BeatSaver::API::BeatmapDownloadInfo> ret;
            for (auto& hash : hashes)
                ret.emplace_back(GetDownload(hash));
            return ret;
        }

        std::vector<std::string> hashes;
    };

    void DownloadMissingSongsFromPlaylist(Playlist* playlist, std::function<void()> onFinished, std::function<void(int, int)> onProgress) {
        // find all the songs needing downloads
        std::vector<std::string> songsToGet;
        for (auto& song : playlist->playlistJSON.Songs) {
            bool hasSong = false;
            // same as PlaylistHasMissingSongs
            ArrayW<BeatmapLevel*> levelList(playlist->playlistCS->_beatmapLevels);
            for (int i = 0; i < levelList.size(); i++) {
                if (MetaCore::Strings::IEquals(song.LevelID, levelList[i]->levelID)) {
                    hasSong = true;
                    break;
                }
            }
            if (!hasSong)
                songsToGet.emplace_back(MetaCore::Songs::GetHash(song.LevelID));
        }

        if (songsToGet.empty()) {
            onFinished();
            return;
        }
        onProgress(songsToGet.size(), 0);

        using Retry = WebUtils::RatelimitedDispatcher::RetryOptions;
        auto requester = new WebUtils::RatelimitedDispatcher();
        auto completed = new std::atomic_int(0);

        auto increment = [completed, onProgress, total = songsToGet.size()]() {
            int num = ++(*completed);
            BSML::MainThreadScheduler::Schedule([onProgress = std::move(onProgress), num, total]() { onProgress(total, num); });
        };

        requester->downloader = BeatSaver::API::GetBeatsaverDownloader();
        requester->maxConcurrentRequests = 3;
        requester->onRequestFinished = [requester, increment](bool success, WebUtils::IRequest* request) -> std::optional<Retry> {
            if (!success) {
                auto response = request->TargetResponse;
                auto http = response->HttpCode;
                auto curl = response->CurlStatus;
                logger.error("{} request failed {} {}", request->URL.fullURl(), http, curl);
                if (curl == 0 && (http >= 200 && http < 300))
                    return Retry{std::chrono::milliseconds(100)};
                // not retrying, mark as done
                increment();
                return std::nullopt;
            }
            // add download requests if it was a get request
            if (auto cast = dynamic_cast<GetBeatmapsForDownloadRequest*>(request)) {
                for (auto& download : cast->GetDownloads())
                    requester->AddRequest(BeatSaver::API::CreateDownloadBeatmapRequest(download));
            } else if (auto cast = dynamic_cast<BeatSaver::API::DownloadBeatmapRequest*>(request))
                increment();

            return std::nullopt;
        };
        requester->allFinished = [requester, completed, onFinished = std::move(onFinished)](auto) {
            BSML::MainThreadScheduler::Schedule([requester, completed, onFinished = std::move(onFinished)]() {
                delete requester;
                delete completed;
                onFinished();
            });
        };

        for (int i = 0; i < songsToGet.size(); i += 50) {
            auto span = std::span(songsToGet).subspan(i, std::min(50, (int) songsToGet.size() - i));
            requester->AddRequest(std::make_unique<GetBeatmapsForDownloadRequest>(span));
        }

        requester->StartDispatchIfNeeded();
    }

    void RemoveMissingSongsFromPlaylist(Playlist* playlist) {
        // store exisiting songs in a new vector to replace the song list with
        std::vector<BPSong> existingSongs = {};
        for (auto& song : playlist->playlistJSON.Songs) {
            if (MetaCore::Songs::FindLevel(song.LevelID))
                existingSongs.push_back(song);
            else if (song.SongName.has_value())
                LOG_INFO("Removing song {} from playlist {}", song.SongName.value(), playlist->name);
            else
                LOG_INFO("Removing song with id {} from playlist {}", song.LevelID, playlist->name);
        }
        // set the songs of the playlist to only those found
        playlist->playlistJSON.Songs = existingSongs;
        playlist->Save();
    }

    void AddSongToPlaylist(Playlist* playlist, BeatmapLevel* level) {
        if (!level)
            return;
        // add song to cs object
        auto& pack = playlist->playlistCS;
        if (!pack)
            return;
        ArrayW<BeatmapLevel*> levelList(pack->_beatmapLevels);
        ArrayW<BeatmapLevel*> newLevels(levelList.size() + 1);
        for (int i = 0; i < levelList.size(); i++) {
            auto currentLevel = levelList[i];
            if (currentLevel->levelID->Equals(level->levelID))
                return;
            newLevels[i] = currentLevel;
        }
        newLevels[levelList.size()] = level;

        pack->SetLevels(newLevels);
        // update json object
        auto& json = playlist->playlistJSON;
        // add a blank song
        auto& songJson = json.Songs.emplace_back();
        // set info
        songJson.Hash = MetaCore::Songs::GetHash(level);
        songJson.LevelID = (std::string) level->levelID;
        songJson.SongName = level->songName;
        // write to file
        playlist->Save();
    }

    void RemoveSongFromPlaylist(Playlist* playlist, BeatmapLevel* level) {
        if (!level)
            return;
        // remove song from cs object
        auto& pack = playlist->playlistCS;
        if (!pack)
            return;
        ArrayW<BeatmapLevel*> levelList(pack->_beatmapLevels);
        if (levelList.size() == 0)
            return;
        ArrayW<BeatmapLevel*> newLevels(levelList.size() - 1);
        // remove only one level if duplicates
        bool removed = false;
        for (int i = 0; i < newLevels.size(); i++) {
            // comparison should work
            auto currentLevel = levelList[removed ? i + 1 : i];
            if (currentLevel->levelID->Equals(level->levelID)) {
                removed = true;
                i--;
            } else
                newLevels[i] = currentLevel;
        }
        if (!removed && !levelList->Last()->levelID->Equals(level->levelID)) {
            LOG_ERROR("Could not find song to be removed!");
            return;
        }

        pack->SetLevels(newLevels);
        // update json object
        auto& json = playlist->playlistJSON;
        // find song by id and remove
        for (auto itr = json.Songs.begin(); itr != json.Songs.end(); ++itr) {
            auto& song = *itr;
            if (MetaCore::Strings::IEquals(song.LevelID, level->levelID)) {
                json.Songs.erase(itr);
                // only erase
                break;
            }
        }
        // write to file
        playlist->Save();
    }

    void RemoveSongFromAllPlaylists(BeatmapLevel* level) {
        for (auto& pair : path_playlists)
            RemoveSongFromPlaylist(pair.second, level);
    }

    void SetSongIndex(Playlist* playlist, BeatmapLevel* level, int index) {
        if (!level)
            return;
        // remove song from cs object
        auto& pack = playlist->playlistCS;
        if (!pack)
            return;
        ArrayW<BeatmapLevel*> levelList(pack->_beatmapLevels);
        if (index >= levelList.size() || index < 0)
            return;
        ArrayW<BeatmapLevel*> newLevels(levelList.size());
        bool found = false;
        // ensure we traverse the whole of both lists
        for (int i = 0, j = 0; i < newLevels.size() || j < levelList.size(); i++) {
            // skip past level in original list, but only the first time
            if (j < levelList.size() && levelList[j]->levelID->Equals(level->levelID) && !found) {
                j++;
                found = true;
            }
            // shift backwards in original list when inserting level
            if (i == index) {
                j--;
                newLevels[i] = level;
            } else if (i < newLevels.size())
                newLevels[i] = levelList[j];
            j++;
        }
        if (found)
            pack->SetLevels(newLevels);
        else {
            LOG_ERROR("Could not find song to be moved!");
            return;
        }
        // update json object
        auto& songs = playlist->playlistJSON.Songs;
        // find songs by id
        int removeIndex = -1;
        auto replacedLevelID = levelList[index]->levelID;
        int replacedLevelIndex = -1;
        for (int i = 0; i < songs.size(); i++) {
            auto& song = songs[i];
            if (MetaCore::Strings::IEquals(song.LevelID, level->levelID))
                removeIndex = i;
            if (MetaCore::Strings::IEquals(song.LevelID, replacedLevelID))
                replacedLevelIndex = i;
            if (removeIndex >= 0 && replacedLevelIndex >= 0)
                break;
        }
        // preserve existing song entry
        auto songJson = songs[removeIndex];
        if (!songJson.SongName.has_value())
            songJson.SongName = level->songName;
        songs.erase(songs.begin() + removeIndex);
        // insert it such that the entry is now at the previous index of what it replaced
        songs.insert(songs.begin() + replacedLevelIndex, songJson);
        // write to file
        playlist->Save();
    }
}
