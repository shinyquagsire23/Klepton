#pragma once

#include "bshook-utils.hpp"
#include <stdint.h>
#include <vector>
#include <future>
#include <istream>
#include <chrono>
#include <SongProto.pb.h>

#include "Data/Song.hpp"

namespace SongDetailsCache {
    template<typename T>
    using shared_ptr_vector = std::shared_ptr<std::vector<T>>;

    template<typename T>
    inline shared_ptr_vector<T> make_shared_vec() { return std::make_shared<std::vector<T>>(); }

    template<typename T, typename U>
    using shared_ptr_unordered_map = std::shared_ptr<std::unordered_map<T, U>>;

    template<typename T, typename U>
    inline shared_ptr_unordered_map<T, U> make_shared_unordered_map() { return std::make_shared<std::unordered_map<T, U>>(); }

    namespace Structs {
        class SongProtoContainer;
    }
    #pragma pack(push, 1)
    struct SongHash {
        public:
            const uint32_t c1 = 0;
            const uint64_t c2 = 0;
            const uint64_t c3 = 0;
            operator const uint8_t*() const noexcept { return reinterpret_cast<const uint8_t*>(&c1); }

            SongHash(const std::string& str);
            bool operator ==(const SongHash& other) const noexcept { return c1 == other.c1 && c2 == other.c2 && c3 == other.c3; }
        private:
#if SC2_DIAGNOSTICS
            friend struct Sc2BoundaryTests;
#endif
            SongHash();
            friend class SongDetailsContainer;
            friend class HexUtil;
    };
    #pragma pack(pop)
    static_assert(sizeof(SongHash) == (sizeof(uint8_t) * 20), "SongHashes should be 20 bytes");

    class SongDetailsContainer {
        public:
            static std::future<void> Load(bool reload = false, int acceptableAgeHours = 1);
        private:
#if SC2_DIAGNOSTICS
            friend struct Sc2BoundaryTests;
#endif
            friend struct Song;
            friend struct SongDifficulty;
            friend struct SongHash;
            friend class SongDetails;
            friend class HexUtil;
            friend class SongArray;
            friend class DiffArray;
            friend class TagsMap;

            static constexpr const int HASH_SIZE_BYTES = 20;
            static_assert(HASH_SIZE_BYTES == sizeof(SongHash), "Song hashes should be 20 bytes");
            static shared_ptr_vector<uint32_t> keys;
            static shared_ptr_vector<SongHash> hashBytes;
            static shared_ptr_vector<uint32_t> hashBytesLUT;
            static shared_ptr_vector<std::string> songNames;
            static shared_ptr_vector<std::string> songAuthorNames;
            static shared_ptr_vector<std::string> levelAuthorNames;
            static shared_ptr_vector<std::string> uploaderNames;
            static shared_ptr_unordered_map<std::string, uint64_t> tags;

            static std::chrono::sys_seconds scrapeEndedTimeUnix;
            static std::chrono::seconds updateThrottle;

            static shared_ptr_vector<Song> songs;
            static shared_ptr_vector<SongDifficulty> difficulties;

            static bool get_isDataAvailable() { return songs && !songs->empty(); }

            static BSHookUtils::UnorderedEventCallback<> dataAvailableOrUpdatedInternal;

            static BSHookUtils::UnorderedEventCallback<std::string> dataLoadFailedInternal;

            static void Load_internal(bool reload, int acceptableAgeHours);
            static void Process(const std::vector<uint8_t>& data, bool force = true);
            static void Process(std::istream& istream, bool force = true);
            static void Process(const Structs::SongDetailsV3& parsedContainer, bool force = true);
    };
}