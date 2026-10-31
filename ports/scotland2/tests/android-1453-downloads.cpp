// Test-only consumers of the original guest APIs; never packaged with the app.
#include <cstdio>
#include <span>
#include <optional>
#include <vector>
#include <cmath>
#include <stdexcept>
#include "web-utils/shared/WebUtils.hpp"
#include "song-details/include/Utils.hpp"
#include "Utils/WebUtils.hpp"
#include "beatsaverplusplus/shared/BeatSaver.hpp"
static long (*browser_get)(std::string,long,std::string&);
static decltype(&SongDetailsCache::WebUtil::GetAsync) details_get;
static int (*browser_extract)(const char*,size_t,const char*,int(*)(const char*,void*),void*);
// The production helpers are intentionally hidden. The host supplies measured
// guest addresses through this flat boundary; all C++ objects stay in guests.
extern "C" __attribute__((visibility("default")))
void sc2_test_1453_bind_private(void* get,void* details,void* extract) {
    browser_get=reinterpret_cast<decltype(browser_get)>(get);
    details_get=reinterpret_cast<decltype(details_get)>(details);
    browser_extract=reinterpret_cast<decltype(browser_extract)>(extract);
}
extern "C" __attribute__((visibility("default")))
int sc2_test_1453_download_tls(const char* url,int good) {
    WebUtils::DownloaderUtility downloader{.userAgent="Scotland2BoundaryTest",.timeOut=10};
    WebUtils::URLOptions options{url,false}; // even legacy callers must verify TLS
    options.noEscape=true;
    auto response=downloader.Get<WebUtils::DataResponse>(std::move(options));
    const auto bytes=response.responseData?response.responseData->size():0;
    fprintf(stderr,"1453_WEBUTILS_TLS good=%d curl=%d http=%d bytes=%zu\n",good,response.get_CurlStatus(),response.get_HttpCode(),bytes);
    if(good ? !(response.IsSuccessful() && bytes) : !(response.get_CurlStatus()==60 && !response.IsSuccessful() && !bytes))return 1;
    auto details=details_get(url,10,{}).get();
    fprintf(stderr,"1453_SONGDETAILS_TLS good=%d curl=%d http=%ld bytes=%zu\n",good,details.curlStatus,details.httpCode,details.content.size());
    if(good ? !(details.httpCode==200 && details.curlStatus==CURLE_OK && !details.content.empty()) :
              !(details.curlStatus==CURLE_PEER_FAILED_VERIFICATION && details.content.empty()))return 2;
    std::string body;
    auto code=browser_get(std::string(url),10,body);
    fprintf(stderr,"1453_SONGDOWNLOADER_TLS good=%d http=%ld bytes=%zu\n",good,code,body.size());
    if(good ? !(code==200 && !body.empty()) : !(code==0 && body.empty()))return 3;
    return 0;
}
extern "C" __attribute__((visibility("default")))
int sc2_test_1453_beatsaver_extract(const char* bytes,size_t size,const char* dest) {
    auto output=std::filesystem::path(dest);
    BeatSaver::API::Init(output.parent_path());
    BeatSaver::API::BeatmapDownloadInfo info{"fixture","https://localhost/unused",output.filename().string()};
    BeatSaver::API::DownloadBeatmapResponse response(info);
    return response.AcceptData(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(bytes),size))?0:1;
}
extern "C" __attribute__((visibility("default")))
int sc2_test_1453_downloader_extract(const char* bytes,size_t size,const char* dest) {
    return browser_extract(bytes,size,dest,nullptr,nullptr);
}
