#include "Formatters.hpp"

#include "logging.hpp"

namespace BetterSongSearch::Formatters {
    StringW FormatMapStyle(StringW value) {
        DEBUG("Formatting map style");
        // Uppercase the first letter
        std::string result = (std::string) value;
        if (result.size() == 0) {
            return result;
        }
        result[0] = std::toupper(result[0]);
        DEBUG("Formatted map style: {}", result);
        return result;
    }

    std::string FormatSongGenre(StringW value) {
        return (std::string) value;
    }
}  // namespace BetterSongSearch::Formatters
