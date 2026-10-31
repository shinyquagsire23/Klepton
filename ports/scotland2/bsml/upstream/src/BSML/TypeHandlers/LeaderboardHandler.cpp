#include "BSML/TypeHandlers/LeaderboardHandler.hpp"

namespace BSML {
    static LeaderboardHandler leaderboardHandlerHandler{};

    LeaderboardHandler::Base::PropMap LeaderboardHandler::get_props() const {
        return {
            {"cellSize", {"cell-size"}}
        };
    }

    LeaderboardHandler::Base::SetterMap LeaderboardHandler::get_setters() const {
        return {
            {"cellSize", [](auto component, auto value){ component->_rowHeight = value; }}
        };
    }
}
