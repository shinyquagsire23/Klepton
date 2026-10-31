#include "beatsaber-hook/shared/utils/utils-functions.h"
#include "Exceptions.hpp"

namespace SongDownloader {
    JsonException::JsonException(SongDownloader::Exceptions key, const std::string& _message) : message(_message) {
        switch (key) {
        case Exceptions::SiteError:
            error = message;
            break;
        case Exceptions::NoMember:
            error = fmt::format("Unexpected Response, expected member in non-optional jsonValue but got false on HasMember!\n JSON Value: {}", message);
            break;
        case Exceptions::WrongType:
            error = fmt::format("Unexpected Response, expected type in non-optional jsonValue does not match!\n JSON Value: {}", message);
            break;
        }
    }
}
