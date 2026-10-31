#include <filesystem>
#include <cstdio>
#include <cstring>
extern "C" __attribute__((visibility("default")))
int sc2_test_recursive_filesystem(const char *root) {
    for(unsigned pass=0;pass<100;++pass) {
        std::error_code ec;
        auto iterator=std::filesystem::recursive_directory_iterator(root,ec);
        if(ec)return 1;
        unsigned count=0;
        for(auto entry:iterator) {
            auto path=entry.path();
            if(entry.is_directory()) {
                (void)std::filesystem::exists(path/"info.dat");
                (void)std::filesystem::exists(path/"Info.dat");
            } else if(!entry.is_regular_file())return 2;
            ++count;
        }
        if(count!=12){std::fprintf(stderr,"FILESYSTEM_COUNT pass=%u count=%u\n",pass,count);return 3;}
    }
    std::fprintf(stderr,"RECURSIVE_FILESYSTEM_OK 100 actual Android libc++ nested directory walks with copied iterators and entry status lookups\n");
    return 0;
}
