#pragma once

#include <fmt/format.h>

#include <string>

#include "export.h"

namespace MetaCore::Strings {
    /// @brief Santizes a path to remove all invalid characters with an aggressive whitelist
    /// @param path The unsanitized path
    /// @return The sanitized path
    METACORE_EXPORT std::string SanitizedPath(std::string const& path);

    /// @brief Finds a unique file name for a file by adding _1, _2, etc (before the extension, if present)
    /// @param file The potentially duplicated filename
    /// @param directory The directory to search for duplicate files in
    /// @return The unique file name (without sanitization)
    METACORE_EXPORT std::string UniqueFileName(std::string const& file, std::string const& directory);

    /// @brief Converts a number of seconds to a string in the form minutes:seconds
    /// @param seconds The total number of seconds
    /// @param hours If hours should be included in the string as well
    /// @return The formatted string
    METACORE_EXPORT std::string SecondsToString(int seconds, bool hours = false);

    /// @brief Converts a time in the past (such as a file modified time) to a string of the largest time unit
    /// @param pastTime The time in the past, in seconds
    /// @return The formatted string, for example "1 day ago"
    METACORE_EXPORT std::string TimeAgoString(long pastTime);

    /// @brief Compares two strings for equality, ignoring case
    /// @param a The first string to compare
    /// @param b The second string to compare
    /// @return If the strings are equal, ignoring case
    METACORE_EXPORT bool IEquals(std::string const& a, std::string const& b);

    /// @brief Converts string to lowercase
    /// @param string The string to convert
    /// @return The string in lowercase
    METACORE_EXPORT std::string Lower(std::string const& string);

    /// @brief Formats a number with a specific number of decimal places
    /// @param value The number to format
    /// @param decimals The fixed number of decimals to write
    /// @return The formatted string
    inline std::string FormatDecimals(double value, int decimals) {
        return fmt::format("{:.{}f}", value, decimals);
    }
}
