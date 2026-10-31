#pragma once

#include "./_config.h"
#include <ranges>
#include <string>
#include <optional>
#include <vector>
#include <thread>

#if defined(WEBUTILS_HAS_RAPIDJSON)
#include "beatsaber-hook/shared/config/rapidjson-utils.hpp"
#endif

#if defined(WEBUTILS_HAS_BSML)
#include "UnityEngine/Sprite.hpp"
#include "UnityEngine/Texture2D.hpp"
// include can be either from bsml directly or from a lib using bsml
#if __has_include("bsml/shared/BSML/Parsing/BSMLParser.hpp")
#include "bsml/shared/BSML/Parsing/BSMLParser.hpp"
#else
#include "BSML/Parsing/BSMLParser.hpp"
#endif

#if __has_include("bsml/shared/BSML/MainThreadScheduler.hpp")
#include "bsml/shared/BSML/MainThreadScheduler.hpp"
#else
#include "BSML/MainThreadScheduler.hpp"
#endif

#if __has_include("bsml/shared/Helpers/utilities.hpp")
#include "bsml/shared/Helpers/utilities.hpp"
#else
#include "Helpers/utilities.hpp"
#endif

#endif

namespace WebUtils {
    class WEBUTILS_EXPORT IResponse {
        public:
            virtual ~IResponse() = default;

            virtual int get_HttpCode() const noexcept = 0;
            virtual void set_HttpCode(int httpCode) noexcept = 0;
            __declspec(property(get=get_HttpCode, put=set_HttpCode)) int HttpCode;

            /// @brief getter for the curl status, 0 means success, anything else means error. This is equivalent to CURLE
            virtual int get_CurlStatus() const noexcept = 0;
            virtual void set_CurlStatus(int curlStatus) noexcept = 0;
            __declspec(property(get=get_CurlStatus, put=set_CurlStatus)) int CurlStatus;

            /// @brief method that will be called on your response to set the data
            virtual bool AcceptData(std::span<uint8_t const> data) = 0;

            /// @brief method that will be called on your response to set the returned header data
            virtual bool AcceptHeaders(std::string_view headers) = 0;

            /// @brief for some returned datatypes, it's worth it to check whether it parsed successfully
            virtual bool DataParsedSuccessful() const noexcept = 0;

            /// @brief check whether the http and curl status were valid
            virtual bool IsSuccessful() const noexcept { return get_HttpCode() >= 200 && get_HttpCode() < 300 && get_CurlStatus() == 0; }
            virtual operator bool() const noexcept { return IsSuccessful(); }
    };

    template<typename T>
    concept response_impl = std::is_base_of_v<IResponse, T> && !std::is_abstract_v<T>;

    /// @brief generic response implementation to use as a base.
    template<typename T>
    struct WEBUTILS_EXPORT GenericResponse : public IResponse {
        int httpCode;
        int curlStatus;
        std::string responseHeaders;
        std::optional<T> responseData;

        virtual int get_HttpCode() const noexcept override { return httpCode; }
        virtual void set_HttpCode(int httpCode) noexcept override { this->httpCode = httpCode; }

        virtual int get_CurlStatus() const noexcept override { return curlStatus; }
        virtual void set_CurlStatus(int curlStatus) noexcept override { this->curlStatus = curlStatus; }

        /// @brief operator to response item
        virtual operator T const&() const { return GetParsedData(); }

        /// @brief gets the response item, will throw if invalid
        /// @throw should throw if response was not valid
        virtual T const& GetParsedData() const { return responseData.value(); };

        /// @brief accepts the headers from the request
        virtual bool AcceptHeaders(std::string_view headers) override { responseHeaders.assign(headers); return true; }

        virtual bool DataParsedSuccessful() const noexcept override { return responseData.has_value(); };
    };

    /// @brief string response, simply reading the data as a string
    struct WEBUTILS_EXPORT StringResponse : public GenericResponse<std::string> {
        virtual bool AcceptData(std::span<uint8_t const> data) override {
            responseData = std::string((char*)data.data(), data.size());
            return true;
        }
    };

    /// @brief string response, simply reading the data as raw data
    struct WEBUTILS_EXPORT DataResponse : public GenericResponse<std::vector<uint8_t>> {
        virtual bool AcceptData(std::span<uint8_t const> data) override {
            std::vector<uint8_t> parsed(data.size());
            std::copy(data.begin(), data.end(), parsed.begin());
            responseData = std::move(parsed);
            return true;
        }
    };

#if defined(WEBUTILS_HAS_RAPIDJSON)
    /// @brief string response, simply reading the data as a json string
    struct WEBUTILS_EXPORT JsonResponse : public GenericResponse<rapidjson::Document> {
        virtual bool AcceptData(std::span<uint8_t const> data) override {
            rapidjson::Document doc;
            doc.Parse((char*)data.data(), data.size());
            if (doc.HasParseError()) return false;
            responseData = std::move(doc);
            return true;
        }
    };
#endif

#if defined(WEBUTILS_HAS_BSML)
    /// @brief string response, simply reading the data as a texture
    struct WEBUTILS_EXPORT TextureResponse : public GenericResponse<UnityW<UnityEngine::Texture2D>> {
        virtual bool AcceptData(std::span<uint8_t const> data) override {
            bool mainThreadRan = false;
            BSML::MainThreadScheduler::Schedule([data, &mainThreadRan, this](){
                ArrayW<uint8_t> imageData(il2cpp_array_size_t(data.size()));
                std::copy(data.begin(), data.end(), imageData.begin());

                auto tex = BSML::Utilities::LoadTextureRaw(imageData);
                if (tex) this->responseData = tex;
                mainThreadRan = true;
            });
            while(!mainThreadRan) std::this_thread::sleep_for(std::chrono::milliseconds(50));
            return responseData.has_value();
        }
    };

    /// @brief string response, simply reading the data as a texture into a sprite
    struct WEBUTILS_EXPORT SpriteResponse : public GenericResponse<UnityW<UnityEngine::Sprite>> {
        virtual bool AcceptData(std::span<uint8_t const> data) override {
            bool mainThreadRan = false;
            BSML::MainThreadScheduler::Schedule([data, &mainThreadRan, this](){
                ArrayW<uint8_t> imageData(il2cpp_array_size_t(data.size()));
                std::copy(data.begin(), data.end(), imageData.begin());

                auto tex = BSML::Utilities::LoadTextureRaw(imageData);
                if (tex) {
                    auto sprite = BSML::Utilities::LoadSpriteFromTexture(tex);
                    if (sprite) this->responseData = sprite;
                }
                mainThreadRan = true;
            });
            while(!mainThreadRan) std::this_thread::sleep_for(std::chrono::milliseconds(50));
            return responseData.has_value();
        }
    };

    /// @brief string response, simply reading & parsing the data as a bsml doc
    struct WEBUTILS_EXPORT BSMLResponse : public GenericResponse<std::shared_ptr<BSML::BSMLParser>> {
        virtual bool AcceptData(std::span<uint8_t const> data) override {
            // ensure null termination, if bsml was smarter about this we wouldn't have to do this
            std::string str((char*)data.data(), data.size());
            responseData = BSML::BSMLParser::parse(str);
            return true;
        }
    };
#endif
}
