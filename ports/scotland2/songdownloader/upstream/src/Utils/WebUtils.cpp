#include "Utils/WebUtils.hpp"

#include "CustomLogger.hpp"

#include "libcurl/shared/curl.h"
#include "libcurl/shared/easy.h"
#include <sstream>
#include <cstdlib>
#include <limits>
#include <algorithm>
#include <cmath>

#define TIMEOUT 10
#define USER_AGENT "QuestSongDownloader/" VERSION " (+https://github.com/darknight1050/SongDownloader)"

namespace WebUtils {
    [[maybe_unused]] static const auto curlInitialized=curl_global_init(CURL_GLOBAL_ALL);
    // Standalone platform trust asset; retain the original curl request code.
    static void ConfigureTLS(CURL* curl) {
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
        curl_easy_setopt(curl, CURLOPT_PROTOCOLS, CURLPROTO_HTTPS);
        curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS, CURLPROTO_HTTPS);
        curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        if (const char* ca=std::getenv("KL_SCOTLAND2_CA_BUNDLE"); ca && *ca)
            curl_easy_setopt(curl, CURLOPT_CAINFO, ca);
    }


    //https://stackoverflow.com/a/55660581
    std::string query_encode(const std::string& s)
    {
        std::stringstream ret;

        #define IS_BETWEEN(ch, low, high) (ch >= low && ch <= high)
        #define IS_ALPHA(ch) (IS_BETWEEN(ch, 'A', 'Z') || IS_BETWEEN(ch, 'a', 'z'))
        #define IS_DIGIT(ch) IS_BETWEEN(ch, '0', '9')
        #define IS_HEXDIG(ch) (IS_DIGIT(ch) || IS_BETWEEN(ch, 'A', 'F') || IS_BETWEEN(ch, 'a', 'f'))

        for(size_t i = 0; i < s.size();)
        {
            char ch = s[i++];

            if (IS_ALPHA(ch) || IS_DIGIT(ch))
            {
                ret << ch;
            }
            else if ((ch == '%') && i + 1 < s.size() && IS_HEXDIG(s[i+0]) && IS_HEXDIG(s[i+1]))
            {
                ret << s.substr(i-1, 3);
                i += 2;
            }
            else
            {
                switch (ch)
                {
                    case '-':
                    case '.':
                    case '_':
                    case '~':
                    case '!':
                    case '$':
                    case '&':
                    case '\'':
                    case '(':
                    case ')':
                    case '*':
                    case '+':
                    case ',':
                    case ';':
                    case '=':
                    case ':':
                    case '@':
                    case '/':
                    case '?':
                    case '[':
                    case ']':
                        ret << ch;
                        break;

                    default:
                    {
                        static const char hex[] = "0123456789ABCDEF";
                        char pct[] = "%  ";
                        pct[1] = hex[(ch >> 4) & 0xF];
                        pct[2] = hex[ch & 0xF];
                        ret << std::string_view(pct, 3);
                        break;
                    }
                }
            }
        }

        return ret.str();
    }


    std::size_t CurlWrite_CallbackFunc_StdString(void *contents, std::size_t size, std::size_t nmemb, std::string *s)
    {
        constexpr std::size_t limit=512ULL*1024*1024;
        if (!s || (size && nmemb>limit/size)) return 0;
        std::size_t newLength = size * nmemb;
        if (s->size()>limit || newLength>limit-s->size()) return 0;
        try {
            s->append((char*)contents, newLength);
        } catch(const std::exception &e) {
            //handle memory problem
            LOG_CRITICAL("Failed to allocate string of size: {}", newLength);
            return 0;
        }
        return newLength;
    }

    std::optional<rapidjson::Document> GetJSON(std::string url) {
        std::string data;
        Get(url, data);
        rapidjson::Document document;
        document.Parse(data);
        if(document.HasParseError() || !document.IsObject())
            return std::nullopt;
        return document;
    }

    long Get(std::string url, std::string& val) {
        return Get(url, TIMEOUT, val);
    }

    long Get(std::string url, long timeout, std::string& val) {
        val.clear();
        // Init curl
        auto* curl = curl_easy_init();
        if (!curl) return 0;
        struct curl_slist *headers = NULL;
        headers = curl_slist_append(headers, "Accept: */*");
        // Set headers
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

        curl_easy_setopt(curl, CURLOPT_URL, query_encode(url).c_str());

        // Don't wait forever, time out after TIMEOUT seconds.
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout);

        // Follow HTTP redirects if necessary.
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "GET");
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlWrite_CallbackFunc_StdString);

        long httpCode(0);

        curl_easy_setopt(curl, CURLOPT_WRITEDATA, reinterpret_cast<void*>(&val));
        curl_easy_setopt(curl, CURLOPT_USERAGENT, USER_AGENT);
        ConfigureTLS(curl);

        curl_easy_setopt(curl, CURLOPT_VERBOSE, 0L);

        auto res = curl_easy_perform(curl);
        /* Check for errors */
        if (res != CURLE_OK) {
            LOG_CRITICAL("curl_easy_perform() failed: {}: {}", int(res), curl_easy_strerror(res));
        }
        if (res == CURLE_OK) curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
        else val.clear();
        curl_easy_cleanup(curl);
        curl_slist_free_all(headers);

        return httpCode;
    }

    struct ProgressUpdateWrapper {
        std::function<void(float)> progressUpdate;
    };

    void GetAsync(std::string url, std::function<void(long, std::string)> finished, std::function<void(float)> progressUpdate) {
        GetAsync(url, TIMEOUT, finished, progressUpdate);
    }

    void GetAsync(std::string url, long timeout, std::function<void(long, std::string)> finished, std::function<void(float)> progressUpdate) {
        std::thread t (
            [url, timeout, progressUpdate, finished] {
                std::string val;
                // Init curl
                auto* curl = curl_easy_init();
                if (!curl) { if(finished) finished(0,{}); return; }
                struct curl_slist *headers = NULL;
                headers = curl_slist_append(headers, "Accept: */*");
                // Set headers
                curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

                curl_easy_setopt(curl, CURLOPT_URL, query_encode(url).c_str());

                // Don't wait forever, time out after TIMEOUT seconds.
                curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout);

                // Follow HTTP redirects if necessary.
                curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

                curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "GET");
                curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlWrite_CallbackFunc_StdString);

                ProgressUpdateWrapper* wrapper = new ProgressUpdateWrapper { progressUpdate };
                if(progressUpdate) {
                    // Internal CURL progressmeter must be disabled if we provide our own callback
                    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, false);
                    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, wrapper);
                    // Install the callback function
                    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION,
                        +[] (void* clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow) {
                            float percentage = dltotal>0 ? std::clamp((float)dlnow / (float)dltotal * 100.0f,0.0f,100.0f) : 0.0f;
                            if(isnan(percentage))
                                percentage = 0.0f;
                            try { reinterpret_cast<ProgressUpdateWrapper*>(clientp)->progressUpdate(percentage); } catch (...) { return 1; }
                            return 0;
                        }
                    );
                }

                long httpCode(0);
                curl_easy_setopt(curl, CURLOPT_WRITEDATA, &val);
                curl_easy_setopt(curl, CURLOPT_USERAGENT, USER_AGENT);
                ConfigureTLS(curl);

                curl_easy_setopt(curl, CURLOPT_VERBOSE, 0L);

                auto res = curl_easy_perform(curl);
                /* Check for errors */
                if (res != CURLE_OK) {
                    LOG_CRITICAL("curl_easy_perform() failed: {}: {}", int(res), curl_easy_strerror(res));
                }
                if (res == CURLE_OK) curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
                else val.clear();
                curl_easy_cleanup(curl);
                curl_slist_free_all(headers);
                delete wrapper;
                if(finished) finished(httpCode, val);
            }
        );
        t.detach();
    }

    void GetJSONAsync(std::string url, std::function<void(long, bool, rapidjson::Document&)> finished) {
        GetAsync(url,
            [finished] (long httpCode, std::string data) {
                rapidjson::Document document;
                document.Parse(data);
                finished(httpCode, document.HasParseError() || !document.IsObject(), document);
            }
        );
    }

}
