// Read Android ELF metadata as data. Mach-O execution belongs to Klepton.
#include "apple-platform.hpp"
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <cstdint>

namespace {
struct Header {
  unsigned char ident[16];
  uint16_t type, machine;
  uint32_t version;
  uint64_t entry, phoff, shoff;
  uint32_t flags;
  uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
};
struct Section {
  uint32_t name, type;
  uint64_t flags, addr, offset, size;
  uint32_t link, info;
  uint64_t alignment, entsize;
};
struct Dynamic { int64_t tag; uint64_t value; };
static_assert(sizeof(Header) == 64 && sizeof(Section) == 64 && sizeof(Dynamic) == 16);
bool contains(size_t total, uint64_t offset, uint64_t size) {
  return offset <= total && size <= total - offset;
}
template<class T> T read(std::vector<char> const& bytes, uint64_t offset) {
  if (!contains(bytes.size(), offset, sizeof(T))) throw std::runtime_error("truncated ELF metadata");
  T result;
  std::memcpy(&result, bytes.data() + offset, sizeof result);
  return result;
}
}

std::vector<std::string> sc2_apple::read_dependencies(std::filesystem::path const& path) {
  std::error_code ec;
  auto size = std::filesystem::file_size(path, ec);
  if (ec || size > 256 * 1024 * 1024) throw std::runtime_error("unreadable or oversized ELF metadata");
  std::ifstream input(path, std::ios::binary);
  if (!input) throw std::runtime_error("cannot open ELF metadata");
  std::vector<char> bytes(static_cast<size_t>(size));
  if (!input.read(bytes.data(), static_cast<std::streamsize>(size)))
    throw std::runtime_error("truncated ELF metadata");
  char extra;
  if (input.get(extra)) throw std::runtime_error("ELF metadata changed size while reading");
  auto header = read<Header>(bytes, 0);
  if (std::memcmp(header.ident, "\177ELF\2\1\1", 7) || header.machine != 183 || header.type != 3 ||
      header.ehsize != sizeof(Header) || header.shentsize != sizeof(Section) || !header.shnum ||
      !contains(bytes.size(), header.shoff, uint64_t(header.shnum) * sizeof(Section)))
    throw std::runtime_error("not supported ARM64 little-endian ELF64 metadata");
  std::vector<std::string> names;
  for (size_t i = 0; i < header.shnum; ++i) {
    auto section = read<Section>(bytes, header.shoff + i * sizeof(Section));
    if (section.type != 6) continue;  // SHT_DYNAMIC
    if (section.entsize != sizeof(Dynamic) || section.size % sizeof(Dynamic) ||
        !contains(bytes.size(), section.offset, section.size) || section.link >= header.shnum)
      throw std::runtime_error("invalid ELF dynamic table");
    auto strings = read<Section>(bytes, header.shoff + section.link * sizeof(Section));
    if (strings.type != 3 || !contains(bytes.size(), strings.offset, strings.size))
      throw std::runtime_error("invalid ELF dynamic string table");
    bool terminated = false;
    for (uint64_t offset = 0; offset < section.size; offset += sizeof(Dynamic)) {
      auto dynamic = read<Dynamic>(bytes, section.offset + offset);
      if (!dynamic.tag) { terminated = true; break; }
      if (dynamic.tag != 1) continue;  // DT_NEEDED
      if (dynamic.value >= strings.size) throw std::runtime_error("ELF dependency string out of range");
      const char *begin = bytes.data() + strings.offset + dynamic.value;
      const char *end = static_cast<const char*>(std::memchr(begin, 0, strings.size - dynamic.value));
      if (!end || end == begin) throw std::runtime_error("invalid ELF dependency string");
      std::string name(begin, end);
      if (name.find('/') != std::string::npos || name.find('\\') != std::string::npos)
        throw std::runtime_error("ELF dependency must be a soname");
      names.emplace_back(std::move(name));
    }
    if (!terminated) throw std::runtime_error("unterminated ELF dynamic table");
  }
  return names;
}
