#pragma once

#include "Data/SongDetailsContainer.hpp"
#include <string>
#include <vector>
#include <span>
#include <future>
#include <unordered_map>

#include "libcurl/shared/curl.h"
#include "libcurl/shared/easy.h"

namespace SongDetailsCache {
    class HexUtil {
        public:
            static std::string ByteArrayToHex(std::span<uint8_t> bytes);
            static std::string SongBytesToHash(std::size_t index);

            /// @throw throws if the passed hex string contains non-hex letters
            static std::vector<uint8_t> ToBytes(std::string_view hex);
            static SongHash ToSongHash(std::string_view hex);
    };

    class WebUtil {
        public:
            struct WebResponse {
                /// @brief http response code
                long httpCode;
                /// @brief curl status code after curl_easy_perform
                CURLcode curlStatus;
                /// @brief http headers
                std::string headers;
                /// @brief http content
                std::string content;

                /// @brief checks for httpCode 200 - 299 and curlStatus CURLE_OK
                inline constexpr operator bool() const noexcept {
                    return httpCode >= 200 && httpCode < 300 && curlStatus == CURLE_OK;
                }
            };

            /// @brief performs an async get request
            /// @param url the url to GET
            /// @param timeout timeout for the request
            /// @param headers headers to pass along with the request
            /// @return async future to await for the response
            static std::future<WebResponse> GetAsync(std::string_view url, uint32_t timeout, const std::unordered_map<std::string, std::string>& headers);
        private:
            static WebResponse GetAsync_internal(std::string url, uint32_t timeout, std::unordered_map<std::string, std::string> headers);
    };
}
