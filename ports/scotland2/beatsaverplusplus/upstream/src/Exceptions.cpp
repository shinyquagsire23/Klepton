#include "Exceptions.hpp"
#include <fmt/core.h>

namespace BeatSaver {
    static std::string_view format_as(ExceptionKind kind) {
        switch (kind) {
            using enum ExceptionKind;
            case SiteError: return "Site Error";
            case NoMember: return "No Member";
            case WrongType: return "Wrong Type";
        }
    }

    JsonException::JsonException(BeatSaver::ExceptionKind kind, const std::string& message) : std::runtime_error(fmt::format("{}: {}", kind, message)) {}
}
