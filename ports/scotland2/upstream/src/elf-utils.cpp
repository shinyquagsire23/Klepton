#include "elf-utils.hpp"
#include "log.h"

#include <link.h>

namespace elf_utils {

  void* getSymbol(std::span<uint8_t> f, std::string_view symbol_name) {
    auto elf = readAtOffset<Elf64_Ehdr>(f, 0);
    LOG_DEBUG("Header read: ehsize: {}, type: {}, version: {}, shentsize: {}", elf.e_ehsize, elf.e_type, elf.e_version,
              elf.e_shentsize);
    auto sections = readManyAtOffset<Elf64_Shdr>(f, elf.e_shoff, elf.e_shnum, elf.e_shentsize);

    Elf64_Shdr const* symtab;
    for (auto const& sectionHeader : sections) {
      if (sectionHeader.sh_type == SHT_SYMTAB) {
        symtab = &sectionHeader;
      }
    }
    auto const* strtab = &sections[symtab->sh_link];

    auto symbols = readManyAtOffset<Elf64_Sym>(f, symtab->sh_offset, symtab->sh_size / symtab->sh_entsize, symtab->sh_entsize);
    for (auto const& symbol : symbols) {
      std::string_view name = &readAtOffset<char const>(f, strtab->sh_offset + symbol.st_name);
      if(symbol_name == name)
        return reinterpret_cast<void*>(symbol.st_value);
    }
    return nullptr;
  }

  uintptr_t baseAddr(char const* soname) {
    if (soname == NULL) return (uintptr_t)NULL;
    struct bdata {
      uintptr_t base;
      char const* soname;
    };
    bdata dat;
    dat.soname = soname;
    int status = dl_iterate_phdr([] (dl_phdr_info* info, size_t, void* data) {
        bdata* dat = reinterpret_cast<bdata*>(data);
        if (std::string(info->dlpi_name).find(dat->soname) != std::string::npos) {
          dat->base = (uintptr_t)info->dlpi_addr;
          return 1;
        }
        return 0;
    }, &dat);
    if(status)
      return dat.base;
    return (uintptr_t)NULL;
  }

}
