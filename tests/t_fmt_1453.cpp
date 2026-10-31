#include <fmt/format.h>
#include "paper2_scotland2/shared/logger.hpp"
#include <cassert>
#include <string_view>

int main() {
#ifdef SC2_TEST_INVALID_FORMAT
    // The build check must reject this; compile-time validation stays enabled.
    (void)fmt::format("{:d}", "text");
#else
    assert(fmt::format("{}{}", std::string_view("left"), "right") == "leftright");
    assert(fmt::format("{:04x}:{}", 42, 17) == "002a:17");
    Paper::FmtStrSrcLoc<int> wrapped("value {} ");
    (void)wrapped;
#endif
}
