#pragma once
#include "../TypeMacros.hpp"

namespace ScoreSaber {
    DECLARE_JSON_STRUCT(Metadata_t) {
        ERROR_CHECK
        GETTER_VALUE(unsigned int, Total, "total");
        GETTER_VALUE(int, Page, "page");
        GETTER_VALUE(int, ItemsPerPage, "itemsPerPage");
    };
}
