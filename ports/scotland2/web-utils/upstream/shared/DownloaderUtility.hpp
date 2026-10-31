#pragma once

#include "./_config.h"
#include "./Response.hpp"
#include <future>
#include <thread>
#include <iterator>
#include <type_traits>

namespace WebUtils {
    struct WEBUTILS_EXPORT URLOptions {
        using QueryMap = std::unordered_map<std::string, std::string>;
        using HeaderMap = std::unordered_map<std::string, std::string>;

        URLOptions(std::string_view url, QueryMap queries, HeaderMap headers, bool useSSL = false, std::string_view encoding = "", std::optional<std::string> userAgent = std::nullopt, std::optional<int> timeOut = std::nullopt) : url(url), queries(queries), headers(headers), useSSL(useSSL), noEscape(false), encoding(encoding), userAgent(userAgent), timeOut(timeOut) {}
        URLOptions(std::string_view url, QueryMap queries, bool useSSL = false, std::string_view encoding = "", std::optional<std::string> userAgent = std::nullopt, std::optional<int> timeOut = std::nullopt) : url(url), queries(queries), headers({}), useSSL(useSSL), noEscape(false), encoding(encoding), userAgent(userAgent), timeOut(timeOut) {}
        URLOptions(std::string_view url, bool useSSL, std::string_view encoding = "", std::optional<std::string> userAgent = std::nullopt, std::optional<int> timeOut = std::nullopt) : url(url), queries({}), headers({}), useSSL(useSSL), noEscape(false), encoding(encoding), userAgent(userAgent), timeOut(timeOut) {}
        URLOptions(std::string_view url, std::optional<std::string> userAgent = std::nullopt, std::optional<int> timeOut = std::nullopt) : url(url), queries({}), headers({}), useSSL(false), noEscape(false), userAgent(userAgent), timeOut(timeOut), encoding("") {}

        /// @brief base url to request from
        std::string url;
        /// @brief queries to append to the URL
        QueryMap queries;
        /// @brief headers to set on the request
        HeaderMap headers;
        /// @brief userAgent to use for the request, if not set uses the downloader utility set useragent
        std::optional<std::string> userAgent;
        /// @brief timeout to use for the request, if not set uses the downloader utility set timeout
        std::optional<int> timeOut;
        /// @brief encoding used for the request, empty string means anything is allowed
        std::string encoding;
        /// @brief whether to verify peers
        bool useSSL;
        /// @brief whether to skip escaping the url, in case you just want your url to be passed raw
        bool noEscape;

        /// @brief formats the url from the set url & queries, also escape
        std::string fullURl() const;

        /// @brief gets whatever is in front of "://" in the url, empty string view otherwise
        std::string_view protocol() const;

        /// @brief checks whether this is a file URL, useful to know
        constexpr bool isFileURL() const noexcept { return protocol() == "file"; }
    };

    struct WEBUTILS_EXPORT DownloaderUtility {
        public:
            std::string userAgent;
            int timeOut;

#pragma region GET
            /// @brief generic get for IResponse classes
            /// @param progressReport progress callback as a float from 0 - 1, allowed to be null
            /// @return whether there was data & it was parsed successfully
            template<response_impl T>
            requires(std::is_default_constructible_v<T>)
            std::future<T> GetAsync(URLOptions urlOptions, std::function<void(float)> progressReport = nullptr) const {
                return std::async(std::launch::any, &DownloaderUtility::Get<T>, this, std::forward<URLOptions>(urlOptions), std::forward<std::function<void(float)>>(progressReport));
            }

            /// @brief generic async get for IResponse classes
            /// @param urlOptions the url options to pass to curl
            /// @param progressReport progress callback as a float from 0 - 1, allowed to be null
            template<response_impl T>
            requires(std::is_default_constructible_v<T>)
            void GetAsync(URLOptions urlOptions, std::function<void(T)> onFinished, std::function<void(float)> progressReport = nullptr) const {
                if (!onFinished) return;

                std::thread([this](URLOptions urlOptions, std::function<void(T)> onFinished, std::function<void(float)> progressReport){
                    onFinished(Get<T>(urlOptions, progressReport));
                }, std::forward<URLOptions>(urlOptions), std::forward<std::function<void(T)>>(onFinished), std::forward<std::function<void(float)>>(progressReport)).detach();
            }

            /// @param progressReport progress callback as a float from 0 - 1, allowed to be null
            template<response_impl T>
            requires(std::is_default_constructible_v<T>)
            T Get(URLOptions urlOptions, std::function<void(float)> progressReport = nullptr) const {
                T response{};
                GetInto(std::forward<URLOptions>(urlOptions), &response, progressReport);
                return response;
            }

            /// @brief gets data from a url synchronously
            /// @param urlOptions the url options to pass to curl
            /// @param targetResponse response to get into
            /// @param progressReport progress callback as a float from 0 - 1, allowed to be null
            /// @return data parsed successfully
            bool GetInto(URLOptions urlOptions, IResponse* targetResponse, std::function<void(float)> progressReport = nullptr) const;

            /// @brief generic get for IResponse classes
            /// @param progressReport progress callback as a float from 0 - 1, allowed to be null
            /// @return whether there was data & it was parsed successfully
            std::future<bool> GetAsyncInto(URLOptions urlOptions, IResponse* targetResponse, std::function<void(float)> progressReport = nullptr) const {
                return std::async(std::launch::any, &DownloaderUtility::GetInto, this, std::forward<URLOptions>(urlOptions), targetResponse, std::forward<std::function<void(float)>>(progressReport));
            }
#pragma endregion // GET

#pragma region POST
            /// @brief generic async post method
            /// @param urlOptions the url options to pass to curl
            /// @param data the data to send. make sure it lives longer than the request takes!
            /// @param progressReport progress callback as a float from 0 - 1, allowed to be null
            /// @return future response
            template<typename T = void>
            requires((response_impl<T> && std::is_default_constructible_v<T>) || std::is_same_v<T, void>)
            std::future<T> PostAsync(URLOptions urlOptions, std::span<uint8_t const> data, std::function<void(float)> progressReport = nullptr) const {
                return std::async(std::launch::any, &DownloaderUtility::Post<T>, this, std::forward<URLOptions>(urlOptions), std::forward<std::span<uint8_t const>>(data), std::forward<std::function<void(float)>>(progressReport));
            }

            /// @brief generic async get for IResponse classes
            /// @param urlOptions the url options to pass to curl
            /// @param data the data to send. make sure it lives longer than the request takes!
            /// @param onFinished method called with the result of the post request, if null the request doesn't happen
            /// @param progressReport progress callback as a float from 0 - 1, allowed to be null
            template<response_impl T>
            requires(std::is_default_constructible_v<T>)
            void PostAsync(URLOptions urlOptions, std::span<uint8_t const> data, std::function<void(T)> onFinished, std::function<void(float)> progressReport = nullptr) const {
                if (!onFinished) return;

                std::thread([this](URLOptions urlOptions, std::span<uint8_t const> data, std::function<void(T)> onFinished, std::function<void(float)> progressReport){
                    onFinished(Post<T>(urlOptions, data, progressReport));
                }, std::forward<URLOptions>(urlOptions), std::forward<std::span<uint8_t const>>(data), std::forward<std::function<void(T)>>(onFinished), std::forward<std::function<void(float)>>(progressReport)).detach();
            }

            /// @brief generic post method
            /// @tparam T expected response type
            /// @param urlOptions the url options to pass to curl
            /// @param data the data to send.
            /// @param progressReport progress callback as a float from 0 - 1, allowed to be null
            /// @return request response
            template<response_impl T>
            requires(std::is_default_constructible_v<T>)
            T Post(URLOptions urlOptions, std::span<uint8_t const> data, std::function<void(float)> progressReport = nullptr) const {
                T response{};
                PostInto(urlOptions, data, &response, progressReport);
                return response;
            }

            /// @brief posts to a url synchronously
            /// @param urlOptions the url options to pass to curl
            /// @param data the data to send.
            /// @param targetResponse post responses may contain response data, this is where it gets parsed into
            /// @param progressReport progress callback as a float from 0 - 1, allowed to be null
            /// @return data parsed successfully
            bool PostInto(URLOptions urlOptions, std::span<uint8_t const> data, IResponse* targetResponse, std::function<void(float)> progressReport = nullptr) const;

            /// @brief posts to a url async
            /// @param urlOptions the url options to pass to curl
            /// @param data the data to send. make sure it lives longer than the request takes!
            /// @param targetResponse post responses may contain response data, this is where it gets parsed into
            /// @param progressReport progress callback as a float from 0 - 1, allowed to be null
            /// @return data parsed successfully
            std::future<bool> PostAsyncInto(URLOptions urlOptions, std::span<uint8_t const> data, IResponse* targetResponse, std::function<void(float)> progressReport = nullptr) {
                return std::async(std::launch::any, &DownloaderUtility::PostInto, this, std::forward<URLOptions>(urlOptions), std::forward<std::span<uint8_t const>>(data), std::forward<IResponse*>(targetResponse), std::forward<std::function<void(float)>>(progressReport));
            }
#pragma endregion // POST
    };
}
