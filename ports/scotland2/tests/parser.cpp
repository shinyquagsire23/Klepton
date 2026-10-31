#include "apple-platform.hpp"
#include <exception>
#include <iostream>
int main(int argc, char **argv) {
  if (argc != 2) return 2;
  try {
    for (auto const& name : sc2_apple::read_dependencies(argv[1])) std::cout << name << '\n';
  } catch (std::exception const& error) {
    std::cerr << error.what() << '\n'; return 1;
  }
}
