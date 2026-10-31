#pragma once

#include "./_config.h"
#include <stdexcept>

namespace BeatSaver {
    enum class BEATSAVER_PLUSPLUS_EXPORT ExceptionKind {
        SiteError, // an error ocurred on the site that was communicated with
        NoMember, // a required member was not found in the returned structure
        WrongType, // type mismatch for a field in the returned structure
    };

    class BEATSAVER_PLUSPLUS_EXPORT JsonException : public std::runtime_error {
        public:
            explicit JsonException(BeatSaver::ExceptionKind kind, const std::string& message);
    };
}
