#pragma once
#include "apple-platform.h"
#include <filesystem>
#include <string>
#include <vector>
namespace sc2_apple {
void *open(const char *path, int flags);
void *symbol(void *handle, const char *name);
const char *error();
int close(void *handle);
bool owns(void *handle, const void *symbol, const char *path);
std::vector<std::string> dependencies(std::filesystem::path const& path);
std::vector<std::string> read_dependencies(std::filesystem::path const& path);
}
