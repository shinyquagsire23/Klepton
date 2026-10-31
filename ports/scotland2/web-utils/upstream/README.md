# WebUtils
A Quest library to simplify downloading various types of content through simple method calls, allowing sync and async method calls

Both GET and POST are supported, find the relevant methods in `web-utils/shared/WebUtils.hpp`

# Downloadable types
 - String as `std::string`
 - Data as `std::vector<uint8_t>`
 - Json as `rapidjson::Document` (requires bs hook)
 - BSML as `std::shared_ptr<BSML::BSMLDocParser>` (requires bsml)
 - Texture2D as `UnityW<UnityEngine::Texture2D>` (requires bsml)
 - Sprite as `UnityW<UnityEngine::Sprite>` (requires bsml)

Some of these require bsml because unity requires unity types to be created on main thread.

## Extendable downloads
You should be able to implement all sort of downloadable types (even your own classes!) by utilizing the `WebUtils::IResponse` class (or `WebUtils::GenericResponse<T>`) to implement a basic class that can be used by WebUtils to download data and parse it into a response. this way you needn't be concerned with invoking curl manually and only worry about implementing a few methods. here's an example implementation of something that parses the returned data as an integer:

```c++
namespace MyMod {
    struct IntegerResponse : public WebUtils::GenericResponse<int> {
        virtual bool AcceptData(std::span<uint8_t const> data) {
            // if data not long enough, return false
            if (data.size() < 4) return false;
            // parse the data as an int
            responseData = *(int32_t*)data.data();
            return true;
        }
    };
}
```

If you're not sure how this is done, I would advise you to have a look over the `web-utils/shared/Response.hpp` header and looking at how webutils has implemented various types

## Disable certain types
If you do not want to expose certain methods from the library to your code (for example, you're getting linker errors because the required library is not linked directly) you can do so with various compile time defines:

 - `WEBUTILS_NO_JSON` disables the rapidjson downloading
 - `WEBUTILS_NO_BSML` disables the sprite, texture and bsml downloading

# Ratelimited downloads
If you are finding yourself running into rate limits or just in general downloads failing for reasons, you can use the `web-utils/shared/RatelimitedDispatcher.hpp` header to send bulk requests in a rate limited fashion. these requests may have any expected `IResponse`, meaning you're not locked in to requesting 1 type per rate limited dispatcher.

A usage example for downloading the google home page mulitple times (weird usecase but whatever)

```c++
#include "web-utils/shared/RatelimitedDispatcher"
#include <iostream>
int main() {
    WebUtils::RatelimitedDispatcher rlDl;
    rlDl.rateLimitTime = std::chrono::milliseconds(1000);
    rlDl.maxConcurrentRequests = 2;
    rlDl.allFinished = [](std::span<std::unique_ptr<WebUtils::IRequest> const> requests) {
        for (int i = 0; auto& req : requests) {
            auto targetResponse = req->TargetResponse;
            auto stringResponse = dynamic_cast<WebUtils::StringResponse*>(targetResponse);
            if (stringResponse->IsSuccessful() && stringResponse->DataParsedSuccessful()) {
                std::cout << "request " << i << ":" << std::endl;
                std::cout << stringResponse->GetParsedData() << std::endl;
            }
        }
    };

    /// add some requests
    rlDl.AddRequest<WebUtils::StringResponse>("https://google.com");
    rlDl.AddRequest<WebUtils::StringResponse>("https://google.com");
    rlDl.AddRequest<WebUtils::StringResponse>("https://google.com");
    rlDl.AddRequest<WebUtils::StringResponse>("https://google.com");

    auto future = rlDl.StartDispatchIfNeeded();
    future.wait();
}
```
