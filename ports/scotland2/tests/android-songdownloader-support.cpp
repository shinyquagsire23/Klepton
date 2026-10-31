#if !defined(SC2_DIAGNOSTICS) || SC2_DIAGNOSTICS
// Exercise the original browser's own networking code, not a substitute client.
#include "Utils/WebUtils.hpp"
#include "BeatSaverAPI.hpp"
#include "zip.h"
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <future>
#include <stdexcept>
#include "CustomTypes/DownloadSongsFlowCoordinator.hpp"
#include "bsml/shared/BSML/FlowCoordinators/MainMenuHolderFlowCoordinator.hpp"
extern "C" __attribute__((visibility("default")))
int sc2_test_songdownloader_request(const char *url) {
    std::string body;
    const auto status=WebUtils::Get(std::string(url),20,body);
    std::fprintf(stderr,"[scotland2] SONGDOWNLOADER_HTTP status=%ld bytes=%zu\n",status,body.size());
    return status==200 && !body.empty()?0:1;
}
extern "C" __attribute__((visibility("default")))
int sc2_test_songdownloader_bad_tls(const char *url) {
    std::string body;
    const auto status=WebUtils::Get(std::string(url),10,body);
    std::fprintf(stderr,"[scotland2] SONGDOWNLOADER_BAD_TLS status=%ld bytes=%zu\n",status,body.size());
    return status==0 && body.empty()?0:1;
}
extern "C" __attribute__((visibility("default")))
int sc2_test_songdownloader_api() {
    auto page=BeatSaver::API::LatestPaged();
    const auto count=page?page->GetDocs().size():0;
    std::fprintf(stderr,"[scotland2] SONGDOWNLOADER_API maps=%zu error=%s\n",count,BeatSaver::API::GetException().c_str());
    return count?0:1;
}
extern "C" __attribute__((visibility("default")))
int sc2_test_songdownloader_safe_extract(const char* bytes, size_t size, const char* dest) {
    return zip_stream_extract(bytes, size, dest, nullptr, nullptr);
}
extern "C" __attribute__((visibility("default")))
int sc2_test_songdownloader_zip(const char *path) {
    auto zip=zip_open(path,0,'r');if(!zip)return 1;
    if(zip_entry_open(zip,"payload.txt")){zip_close(zip);return 2;}
    void *buffer=nullptr;size_t size=0;
    auto result=zip_entry_read(zip,&buffer,&size);
    const char expected[]="SCOTLAND2_REAL_ZIP_DEFLATE\n";
    bool valid=result==static_cast<ssize_t>(size) && buffer && size==(sizeof(expected)-1)*100;
    for(size_t i=0;valid && i<100;++i)
        valid=!std::memcmp(static_cast<char*>(buffer)+i*(sizeof(expected)-1),expected,sizeof(expected)-1);
    std::free(buffer);zip_entry_close(zip);zip_close(zip);
    return valid?0:3;
}
extern "C" __attribute__((visibility("default")))
int sc2_test_songdownloader_callback_safety(const char* url) {
    auto promise=std::make_shared<std::promise<bool>>();auto result=promise->get_future();
    WebUtils::GetAsync(url,10,[promise](long status,std::string body){promise->set_value(status==0 && body.empty());},[](float){throw std::runtime_error("isolated callback failure");});
    return result.get()?0:1;
}
extern "C" __attribute__((visibility("default")))
int sc2_test_songdownloader_late() {
    auto registration=BSML::MainMenuRegistration::get_registration("SongDownloader");
    if(!registration || registration->hoverHint!="More Songs" ||
       registration->csType!=csTypeOf(SongDownloader::DownloadSongsFlowCoordinator*))return 1;
    std::fprintf(stderr,"[scotland2] SONGDOWNLOADER_MENU_REGISTERED_OK original late callback registered the actual SongDownloader flow coordinator\n");
    std::thread([] {
        int result=sc2_test_songdownloader_api();
        std::fprintf(stderr,"[scotland2] SONGDOWNLOADER_STANDALONE_API_%s original browser fetched and parsed BeatSaver using bundled certificate trust; result=%d\n",result?"FAILED":"OK",result);
    }).detach();
    return 0;
}

#endif
