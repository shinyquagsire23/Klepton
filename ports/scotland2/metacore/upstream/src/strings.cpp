#include "strings.hpp"

#include <filesystem>

std::string MetaCore::Strings::SanitizedPath(std::string const& path) {
    std::string newName;
    newName.reserve(path.size());
    // just whitelist simple characters
    static auto const okChar = [](unsigned char c) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
            return true;
        if (c == '_' || c == '-' || c == '.' || c == '/' || c == '(' || c == ')')
            return true;
        return false;
    };
    std::transform(path.begin(), path.end(), std::back_inserter(newName), [](unsigned char c) {
        if (!okChar(c))
            return (unsigned char) ('_');
        return c;
    });
    if (newName == "")
        return "_";
    return newName;
}

std::string MetaCore::Strings::UniqueFileName(std::string const& file, std::string const& directory) {
    std::filesystem::path input = file;
    std::filesystem::path dir = directory;
    std::string stem = input.stem();
    std::string extension = input.extension();
    std::string output = stem + extension;
    int num = 1;
    while (std::filesystem::exists(dir / output))
        output = fmt::format("{}_{}{}", stem, num++, extension);
    return output;
}

std::string MetaCore::Strings::SecondsToString(int seconds, bool hours) {
    int minutes = seconds / 60;
    seconds %= 60;

    if (!hours)
        return fmt::format("{}:{:02}", minutes, seconds);

    int hoursNum = minutes / 60;
    minutes %= 60;
    return fmt::format("{}:{:02}:{:02}", hoursNum, minutes, seconds);
}

std::string MetaCore::Strings::TimeAgoString(long pastTime) {
    namespace c = std::chrono;
    long seconds = c::duration_cast<c::seconds>(c::system_clock::now().time_since_epoch()).count() - pastTime;

    int minutes = seconds / 60;
    int hours = minutes / 60;
    int days = hours / 24;
    int weeks = days / 7;
    int months = weeks / 4;
    int years = weeks / 52;

    std::string unit;
    int value;

    if (years != 0) {
        unit = "year";
        value = years;
    } else if (months != 0) {
        unit = "month";
        value = months;
    } else if (weeks != 0) {
        unit = "week";
        value = weeks;
    } else if (days != 0) {
        unit = "day";
        value = days;
    } else if (hours != 0) {
        unit = "hour";
        value = hours;
    } else if (minutes != 0) {
        unit = "minute";
        value = minutes;
    } else {
        unit = "second";
        value = (int) seconds;
    }

    return fmt::format("{} {}{} ago", value, unit, value == 1 ? "" : "s");
}

bool MetaCore::Strings::IEquals(std::string const& a, std::string const& b) {
    if (a.size() != b.size())
        return false;
    for (int i = 0; i < a.size(); i++) {
        if (tolower(a[i]) != tolower(b[i]))
            return false;
    }
    return true;
}

std::string MetaCore::Strings::Lower(std::string const& string) {
    std::string ret;
    ret.reserve(string.size());
    std::transform(string.begin(), string.end(), std::back_inserter(ret), tolower);
    return ret;
}
