#pragma once

#include "./_config.h"
#include "./Response.hpp"
#include "./DownloaderUtility.hpp"
#include <type_traits>

namespace WebUtils {
    static WEBUTILS_EXPORT DownloaderUtility const downloader{.userAgent = WEBUTILS_USER_AGENT, .timeOut = WEBUTILS_TIMEOUT};

#pragma region GET
    /// @brief runs a get request asynchronously
    /// @tparam T the response type to output
    /// @param urlOptions url options to pass to curl
    /// @param progressReport progress callback as a float from 0 - 1, allowed to be null
    /// @return future response
    template<response_impl T>
    requires(std::is_default_constructible_v<T>)
    inline std::future<T> WEBUTILS_EXPORT GetAsync(URLOptions urlOptions, std::function<void(float)> progressReport = nullptr) {
        return downloader.GetAsync<T>(std::forward<URLOptions>(urlOptions), std::forward<std::function<void(float)>>(progressReport));
    }

    /// @brief runs a get request asynchronously, calling onFinished when done
    /// @tparam T the response type to output
    /// @param urlOptions url options to pass to curl
    /// @param onFinished function to run when done, NOT RAN ON MAIN OR BOUND IL2CPP THREAD
    /// @param progressReport progress callback as a float from 0 - 1, allowed to be null
    template<response_impl T>
    requires(std::is_default_constructible_v<T>)
    inline void WEBUTILS_EXPORT GetAsync(URLOptions urlOptions, std::function<void(T)> onFinished, std::function<void(float)> progressReport = nullptr) {
        downloader.GetAsync<T>(std::forward<URLOptions>(urlOptions), std::forward<std::function<void(T)>>(onFinished), std::forward<std::function<void(float)>>(progressReport));
    }

    /// @brief runs a get request synchronously
    /// @tparam T the response type to output
    /// @param urlOptions url options to pass to curl
    /// @param progressReport progress callback as a float from 0 - 1, allowed to be null
    /// @return response
    template<response_impl T>
    requires(std::is_default_constructible_v<T>)
    inline T WEBUTILS_EXPORT Get(URLOptions urlOptions, std::function<void(float)> progressReport = nullptr) {
        return downloader.Get<T>(std::forward<URLOptions>(urlOptions), std::forward<std::function<void(float)>>(progressReport));
    }
#pragma endregion // GET

#pragma region POST
    /// @brief runs a get request asynchronously
    /// @tparam T the response type to output
    /// @param urlOptions url options to pass to curl
    /// @param data the data to send. make sure it lives longer than the request takes!
    /// @param progressReport progress callback as a float from 0 - 1, allowed to be null
    /// @return future response
    template<response_impl T>
    requires(std::is_default_constructible_v<T>)
    inline std::future<T> WEBUTILS_EXPORT PostAsync(URLOptions urlOptions, std::span<uint8_t const> data, std::function<void(float)> progressReport = nullptr) {
        return downloader.PostAsync<T>(std::forward<URLOptions>(urlOptions), std::forward<std::span<uint8_t const>>(data), std::forward<std::function<void(float)>>(progressReport));
    }

    /// @brief runs a get request asynchronously, calling onFinished when done
    /// @tparam T the response type to output
    /// @param urlOptions url options to pass to curl
    /// @param data the data to send. make sure it lives longer than the request takes!
    /// @param onFinished function to run when done, NOT RAN ON MAIN OR BOUND IL2CPP THREAD
    /// @param progressReport progress callback as a float from 0 - 1, allowed to be null
    template<response_impl T>
    requires(std::is_default_constructible_v<T>)
    inline void WEBUTILS_EXPORT PostAsync(URLOptions urlOptions, std::span<uint8_t const> data, std::function<void(T)> onFinished, std::function<void(float)> progressReport = nullptr) {
        downloader.PostAsync<T>(std::forward<URLOptions>(urlOptions), std::forward<std::span<uint8_t const>>(data), std::forward<std::function<void(T)>>(onFinished), std::forward<std::function<void(float)>>(progressReport));
    }

    /// @brief runs a get request synchronously
    /// @tparam T the response type to output
    /// @param urlOptions url options to pass to curl
    /// @param data the data to send.
    /// @param progressReport progress callback as a float from 0 - 1, allowed to be null
    /// @return response
    template<response_impl T>
    requires(std::is_default_constructible_v<T>)
    inline T WEBUTILS_EXPORT Post(URLOptions urlOptions, std::span<uint8_t const> data, std::function<void(float)> progressReport = nullptr) {
        return downloader.Post<T>(std::forward<URLOptions>(urlOptions), std::forward<std::span<uint8_t const>>(data), std::forward<std::function<void(float)>>(progressReport));
    }
#pragma endregion // POST
}
