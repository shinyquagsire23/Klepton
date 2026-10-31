// Platform replacement for beatsaber-hook 6.4.2's mkpath(string_view).
// Upstream shells out to "mkdir -p -m +rw". Keep the Android C++ argument ABI,
// use guest path-aware mkdir/stat instead, and retain a system-style exit status.
#include <string_view>
#include <string>
#include <sys/stat.h>
#include <cerrno>
#include <initializer_list>

extern "C" __attribute__((visibility("default")))
int sc2_bshook_mkpath(std::string_view view) {
    if(view.empty() || view.find('\0')!=std::string_view::npos) { errno=EINVAL;return 256; }
    std::string path(view);
    // These Android roots are virtual aliases in Klepton. Start at the whole
    // root so we never try to create their unmapped /storage or /mnt parents.
    size_t begin=1;
    for(std::string_view root:{"/sdcard","/storage/emulated/0","/mnt/sdcard"}) {
        if(view.starts_with(root) && (view.size()==root.size() || view[root.size()]=='/')) {
            begin=root.size();break;
        }
    }
    for(size_t end=begin;end<=path.size();++end) {
        if(end<path.size() && path[end]!='/')continue;
        if(path[end-1]=='/')continue;
        char saved=path[end];path[end]='\0';
        int result=mkdir(path.c_str(),0777),why=errno;
        if(result && why==EEXIST) {
            struct stat info;
            if(!stat(path.c_str(),&info)) {
                if(S_ISDIR(info.st_mode))result=0;
                else why=ENOTDIR;
            } else why=errno;
        }
        path[end]=saved;
        if(result){errno=why;return 256;}
    }
    return 0;
}
