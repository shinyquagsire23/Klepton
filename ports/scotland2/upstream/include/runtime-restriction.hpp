#pragma once

#include <string>
#include <vector>

namespace runtime_restriction {

  bool init(std::string_view modloaderFile);

  bool add_ld_library_paths(std::vector<std::string>&& paths);

}