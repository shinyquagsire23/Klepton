#pragma once

#include "System/Version.hpp"

namespace SongCore {
    struct Version {
        int major, minor, patch;
        constexpr Version(int major = 0, int minor = 0, int patch = 0) noexcept : major(major), minor(minor), patch(patch) {}
        explicit Version(System::Version* ver) : major(ver->Major), minor(ver->Minor), patch(ver->Build) {}

        operator System::Version*() const {
            return System::Version::New_ctor(major, minor, patch);
        }

        bool operator <(Version const& rhs) const { 
            if (major < rhs.major) return true;
            if (major == rhs.major && minor < rhs.minor) return true;
            if (major == rhs.major && minor == rhs.minor && patch < rhs.patch) return true;
            return false;
        }
        bool operator <=(Version const& rhs) const {
            if (major == rhs.major && minor == rhs.minor && patch <= rhs.patch) return true;
            if (major == rhs.major && minor <= rhs.minor) return true;
            if (major <= rhs.major) return true;
            return false;
        }
        bool operator ==(Version const& rhs) const { 
            return major == rhs.major && minor == rhs.minor && patch == rhs.patch;
        }
        bool operator >(Version const& rhs) const { 
            if (major > rhs.major) return true;
            if (major == rhs.major && minor > rhs.minor) return true;
            if (major == rhs.major && minor == rhs.minor && patch > rhs.patch) return true;
            return false;
        }
        bool operator >=(Version const& rhs) const { 
            if (major == rhs.major && minor == rhs.minor && patch >= rhs.patch) return true;
            if (major == rhs.major && minor >= rhs.minor) return true;
            if (major >= rhs.major) return true;
            return false;
        }

        static Version noVersion;
    };

    /// @brief parses a version from the filepath that should be pointing to a json file
    Version VersionFromFilePath(std::filesystem::path const& filePath);
    /// @brief parses a version from the data
    Version VersionFromFileData(std::string const& data);
}
