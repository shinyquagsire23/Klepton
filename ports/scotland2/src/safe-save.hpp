#pragma once
// Atomic same-directory replacement. A failed backup may never truncate the
// previous good copy, and backup errors may never unwind through a game hook.
#include <filesystem>
#include <mutex>
#include <atomic>
#include <string>
#include <system_error>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <cerrno>
namespace sc2_save {
inline std::recursive_mutex mutex;
inline bool atomic_copy(const std::filesystem::path& source,const std::filesystem::path& dest) noexcept {
    int in=-1,out=-1;std::filesystem::path temporary;
    try {
        in=open(source.c_str(),O_RDONLY|O_NOFOLLOW);
        struct stat st{};
        if(in<0 || fstat(in,&st) || !S_ISREG(st.st_mode) || st.st_size<0 || st.st_size>64*1024*1024) {
            if(in>=0)close(in);return false;
        }
        std::filesystem::create_directories(dest.parent_path());
        // Native mkstemp cannot translate an Android alias in place: the host
        // path may be longer than the caller's filename buffer. Reserve through
        // the guest-aware open boundary instead, never following/replacing an
        // existing name. Only a file successfully created here is cleaned up.
        static std::atomic<unsigned long long> sequence{0};
        for (unsigned attempt=0; attempt<128; ++attempt) {
            auto candidate = dest.parent_path() / (".sc2-save-" + std::to_string(getpid()) + "-" + std::to_string(sequence.fetch_add(1)));
            out = open(candidate.c_str(), O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW, 0600);
            if (out>=0) { temporary=std::move(candidate); break; }
            if (errno!=EEXIST) break;
        }
        if(out<0){close(in);return false;}
        char buffer[65536];off_t copied=0;
        for(;;){
            ssize_t n=read(in,buffer,sizeof buffer);
            if(n<0 && errno==EINTR)continue;
            if(n<0)throw std::system_error(errno,std::generic_category());
            if(!n)break;
            if(copied>st.st_size-n)throw std::runtime_error("save changed during backup");
            for(ssize_t i=0;i<n;){ssize_t w=write(out,buffer+i,n-i);if(w<0 && errno==EINTR)continue;if(w<=0)throw std::runtime_error("save write failed");i+=w;}
            copied+=n;
        }
        if(copied!=st.st_size || fsync(out))throw std::runtime_error("incomplete save copy");
        int closed=close(out);out=-1;
        if(closed)throw std::runtime_error("save close failed");
        close(in);in=-1;
        std::filesystem::rename(temporary,dest);return true;
    } catch(...) {
        if(in>=0)close(in);if(out>=0)close(out);
        if(!temporary.empty()){std::error_code ec;std::filesystem::remove(temporary,ec);}
        return false;
    }
}
inline bool should_restore(const std::filesystem::path& backup,const std::filesystem::path& dest) {
    if(!std::filesystem::is_regular_file(std::filesystem::symlink_status(backup)))return false;
    auto status=std::filesystem::symlink_status(dest);
    if(std::filesystem::is_symlink(status) || std::filesystem::is_directory(status))return false;
    if(!std::filesystem::exists(status))return true;
    // A stale backup must not undo a successful newer live save.
    return std::filesystem::last_write_time(backup)>=std::filesystem::last_write_time(dest);
}
}
