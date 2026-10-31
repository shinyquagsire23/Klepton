#include "Util/TextUtil.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <cctype>
#include <ranges>
#include <string>
#include <vector>

namespace BetterSongSearch::Util {
    // this hurts
    std::vector<std::string> split(std::string_view buffer, std::string_view const delimeter) {
        std::vector<std::string> ret{};
        std::decay_t<decltype(std::string::npos)> pos{};
        while ((pos = buffer.find(delimeter)) != std::string::npos) {
            auto const match = buffer.substr(0, pos);
            if (!match.empty()) {
                ret.emplace_back(match);
            }
            buffer = buffer.substr(pos + delimeter.size());
        }
        if (!buffer.empty()) {
            ret.emplace_back(buffer);
        }
        return ret;
    }

    std::string join(std::vector<std::string> strings, std::string_view const delimeter) {
        if (strings.empty()) {
            return "";
        }

        std::string ret;
        for (size_t i = 0; i < strings.size(); i++) {
            ret += strings[i];
            if (i != strings.size() - 1) {
                ret += delimeter;
            }
        }
        return ret;
    }

    /**
     * Removes special characters from a string
     */
    std::string removeSpecialCharacter(std::string_view const s) {
        auto filteredView = s | std::views::filter([](char c) {
                                return std::isalnum(c) || c == ' ';
                            });
        std::string stringy(filteredView.begin(), filteredView.end());
        return stringy;
    }

    std::string toLower(std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), ::tolower);
        return s;
    }

    std::string toLower(std::string_view s) {
        return toLower(std::string(s));
    }

    std::string toLower(char const* s) {
        return toLower(std::string(s));
    }

    bool IsSpace(char x) {
        return x == ' ' || !isalnum(x);
    };

    std::string httpErrorToString(int code) {
        switch (code) {
            case 400:
                return "Bad Request";
            case 401:
                return "Unauthorized";
            case 403:
                return "Forbidden";
            case 404:
                return "Not Found";
            case 405:
                return "Method Not Allowed";
            case 429:
                return "Too Many Requests";
            case 500:
                return "Internal Server Error";
            case 502:
                return "Bad Gateway";
            case 503:
                return "Service Unavailable";
            case 504:
                return "Gateway Timeout";
            default:
                return fmt::format("HTTP Error {}", code);
        }
    }

    std::string curlErrorToString(int code) {
        switch (code) {
            case 0:  // CURLE_OK
                return "No error";
            case 1:  // CURLE_UNSUPPORTED_PROTOCOL
                return "Unsupported protocol";
            case 2:  // CURLE_FAILED_INIT
                return "Failed to initialize cURL";
            case 3:  // CURLE_URL_MALFORMAT
                return "Malformed URL format";
            case 6:  // CURLE_COULDNT_RESOLVE_HOST
                return "Couldn't resolve host";
            case 7:  // CURLE_COULDNT_CONNECT
                return "Couldn't connect to server";
            case 16:  // CURLE_HTTP2
                return "HTTP/2 framing layer error";
            case 28:  // CURLE_OPERATION_TIMEDOUT
                return "Operation timed out";
            case 35:  // CURLE_SSL_CONNECT_ERROR
                return "SSL connection error";
            case 52:  // CURLE_GOT_NOTHING
                return "Server returned nothing";
            case 78:  // CURLE_REMOTE_FILE_NOT_FOUND
                return "Remote file not found";

            default:
                return fmt::format("Code {}", code);
        }
    }
}  // namespace BetterSongSearch::Util
