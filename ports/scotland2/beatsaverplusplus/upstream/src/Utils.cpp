#include "Utils.hpp"
#include "zip/shared/zip.h"
#include "web-utils/shared/DownloaderUtility.hpp"

#include <algorithm>
#include <fstream>
#include <vector>

namespace BeatSaver::Utils {
    static const WebUtils::DownloaderUtility downloader{.userAgent=MOD_ID "/" VERSION " (+https://github.com/RedBrumbler/BeatSaverPlusPlus)"};

    std::vector<char> make_vec(std::string_view chars) {
        std::vector<char> vec{chars.begin(), chars.end()};
        std::sort(vec.begin(), vec.end());
        return vec;
    }

    std::string ReplaceIllegalCharsInPath(std::string path) {
        static const auto allowedCharacters = make_vec("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ1234567890()[]{}%&.:,;=!-_ ");
        for (auto& c : path) {
            // if we dont find the char in the allowed chars, turn it into _
            if (!std::ranges::binary_search(allowedCharacters, c)) c = '_';
        }
        return path;
    }

    bool ExtractAll(std::span<uint8_t const> zipData, std::filesystem::path const& outputPath) {
        zip_error_t error;
        zip_error_init(&error);

        auto zipSrc = zip_source_buffer_create(zipData.data(), zipData.size(), 0, &error);
        if (zipSrc == nullptr) {
            // could not create src buffer
            zip_error_fini(&error);
            return false;
        }

        auto zip = zip_open_from_source(zipSrc, ZIP_RDONLY, &error);
        if (zip == nullptr) {
            // was not a correct zip
            zip_error_fini(&error);
            zip_source_free(zipSrc);
            zipSrc = nullptr;
            return false;
        }

        auto fileCount = zip_get_num_files(zip);
        for (auto i = 0; i < fileCount; i++) {
            auto name = zip_get_name(zip, i, ZIP_FL_ENC_GUESS);
            if (!name) continue;
            auto f = zip_fopen_index(zip, i, ZIP_FL_UNCHANGED);
            if (!f) continue;
            std::filesystem::path outputFilePath = outputPath / name;
            std::filesystem::create_directories(outputFilePath.parent_path());

            std::ofstream of(outputFilePath, std::ios::out | std::ios::binary);
            char buf[256];
            int64_t read = 0;
            while ((read = zip_fread(f, buf, 256)) > 0) {
                of.write(buf, read);
            }

            zip_fclose(f);
            of.close();
        }

        return true;
    }

    std::optional<std::vector<uint8_t>> GetData(std::string dataURL) {
        return downloader.Get<WebUtils::DataResponse>({dataURL}).responseData;
    }
}
