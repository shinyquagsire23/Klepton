#pragma once

#include <filesystem>
#include <fstream>
#include <future>
#include <unordered_map>
#include <istream>

namespace SongDetailsCache {
    class DataGetter {
        public:
            struct DownloadedDatabase {
                std::string source;
                std::string etag;
                std::shared_ptr<std::vector<uint8_t>> data;
            };

            static const std::unordered_map<std::string, std::string> dataSources;
            static std::filesystem::path cachePath();
            static std::filesystem::path cachePathEtag(std::string_view source);

            static std::future<std::optional<DownloadedDatabase>> UpdateAndReadDatabase(std::string_view dataSourceName = "Direct");
            static std::future<void> WriteCachedDatabase(DownloadedDatabase& db);
            static std::optional<std::ifstream> ReadCachedDatabase();
            static bool HasCachedData(int maximumAgeHours = 12);
        private:
#if SC2_DIAGNOSTICS
            friend struct Sc2BoundaryTests;
#endif
            static std::filesystem::path basePath;
            friend class SongDetails;
            static void WriteCachedDatabase_internal(DownloadedDatabase& db);
            static bool DecompressBrotli(std::vector<uint8_t>& out, std::string const& in);
            static std::optional<DownloadedDatabase> UpdateAndReadDatabase_internal(std::string_view dataSourceName = "Direct");
    };
}