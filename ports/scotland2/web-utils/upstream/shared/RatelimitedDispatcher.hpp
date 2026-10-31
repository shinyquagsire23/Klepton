#pragma once

#include "./_config.h"
#include "DownloaderUtility.hpp"
#include "Response.hpp"
#include <atomic>
#include <shared_mutex>
#include <chrono>
#include <queue>
#include <type_traits>

namespace WebUtils {
    class WEBUTILS_EXPORT IRequest {
        public:
            virtual ~IRequest() = default;

            virtual URLOptions const& get_URL() const = 0;
            virtual IResponse* get_TargetResponse() = 0;
            virtual IResponse const* get_TargetResponse() const = 0;

            __declspec(property(get=get_TargetResponse)) IResponse* TargetResponse;
            __declspec(property(get=get_URL)) URLOptions const& URL;
    };

    template<response_impl T>
    requires(std::is_default_constructible_v<T>)
    struct WEBUTILS_EXPORT GenericRequest : public IRequest {
        GenericRequest(URLOptions url) : url(url) {}
        URLOptions url;
        T targetResponse{};

        virtual URLOptions const& get_URL() const override { return url; }
        virtual IResponse* get_TargetResponse() override { return &targetResponse; };
        virtual IResponse const* get_TargetResponse() const override { return &targetResponse; };
    };

    /// @brief struct to make sending multiple requests easier when dealing with rate limits
    /// simply set your limits, set the values for the downloader, and run the requests
    struct WEBUTILS_EXPORT RatelimitedDispatcher {
        public:
            virtual ~RatelimitedDispatcher() = default;

            DownloaderUtility downloader{.userAgent = WEBUTILS_USER_AGENT, .timeOut = WEBUTILS_TIMEOUT};

            std::size_t maxConcurrentRequests = 1;
            std::chrono::milliseconds rateLimitTime = std::chrono::milliseconds(0);

            /// @brief struct used for when a response is complete and it may need to be retried
            struct RetryOptions {
                /// @brief time to wait before reattempting
                std::chrono::milliseconds waitTime;
            };

            /// @brief method invoked when a request has finished
            /// @param success whether the request was succesful (no http/curl errors, deserialize of response worked)
            /// @param req unique pointer to request, non owning pointer
            /// @return optional retry options. if set retries the request
            std::function<std::optional<RetryOptions>(bool success, IRequest* response)> onRequestFinished;

            /// @brief method invoked when all requests have been finished, including ones added while requests were going
            /// @brief readonly span of the performed requests
            std::function<void(std::span<std::unique_ptr<IRequest> const> requests)> allFinished;

            /// @brief gets whether there are any requests to dispatch
            bool AnyRequestsToDispatch();
            /// @brief gets the size of the request queue
            std::size_t RequestCountToDispatch();

            /// @brief adds a request onto the queue
            void AddRequest(std::unique_ptr<IRequest> req);

            /// @brief gets the next request from the queue
            /// @throw throws on invalid pop
            std::unique_ptr<IRequest> PopRequest();

            /// @brief adds a request onto the queue
            template<response_impl T>
            void AddRequest(URLOptions urlOptions) {
                AddRequest(std::make_unique<GenericRequest<T>>(urlOptions));
            }

            /// @brief adds all url options as T requests onto the queue
            /// @tparam response type to parse into
            /// @param options span of url options to use
            template<response_impl T>
            void AddRequests(std::span<URLOptions const> options) {
                for (auto& url : options) {
                    AddRequest(std::make_unique<GenericRequest<T>>(url));
                }
            }

            /// @brief starts the dispatch of the type when required
            /// @return currently executing future so you can await its completion
            std::shared_future<void> StartDispatchIfNeeded();
        protected:
            /// @brief method called when a request has finished
            /// @param success whether the request was succesful (no http/curl errors, deserialize of response worked)
            /// @param req unique pointer to request, non owning pointer
            /// @return optional retry options. if set retries the request
            virtual std::optional<RetryOptions> RequestFinished(bool success, IRequest* req) const;

            /// @brief method called when all requests have finished (queue empty)
            virtual void AllFinished(std::span<std::unique_ptr<IRequest> const> finishedRequests);
        private:
            /// @brief mutex used to guard accesses to the requests queue
            std::shared_mutex _requestsMutex;
            /// @brief queue used to store the requests to be done
            std::queue<std::unique_ptr<IRequest>> _requestsToDispatch;
            /// @brief mutex used to guard accesses to the finished requests vector
            std::shared_mutex _finishedMutex;
            /// @brief vector used to store finished requests
            std::vector<std::unique_ptr<IRequest>> _finishedRequests;

            /// @brief the currently executing dispatch
            std::shared_future<void> _currentRateLimitDispatch;

            /// @brief dispatcher thread
            void DispatcherThread();

            /// @brief dispatcher worker
            void DispatchWorker();
    };
}
