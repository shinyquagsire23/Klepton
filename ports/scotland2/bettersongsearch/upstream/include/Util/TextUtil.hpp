#pragma once

#include <vector>
#include <string>
#include <algorithm>

namespace BetterSongSearch::Util { 
    // this hurts
    std::vector<std::string> split(std::string_view buffer, const std::string_view delimeter = " ");

    // @brief Joins a vector of strings into a single string, separated by a delimeter
    std::string join(std::vector<std::string> strings, const std::string_view delimeter = " ");

    /**
     * Removes special characters from a string
    */
    std::string removeSpecialCharacter(std::string_view const s);
    std::string toLower(std::string s);
    std::string toLower(std::string_view s);
    std::string toLower(char const* s);

    bool IsSpace(char x);

    std::string httpErrorToString(int code);
    std::string curlErrorToString(int code);
}
