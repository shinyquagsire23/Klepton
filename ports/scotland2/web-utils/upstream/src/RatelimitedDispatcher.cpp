#include "RatelimitedDispatcher.hpp"

namespace WebUtils {
    bool RatelimitedDispatcher::AnyRequestsToDispatch() {
        std::shared_lock lock(_requestsMutex);
        return !_requestsToDispatch.empty();
    }

    std::size_t RatelimitedDispatcher::RequestCountToDispatch() {
        std::shared_lock lock(_requestsMutex);
        return _requestsToDispatch.size();
    }

    void RatelimitedDispatcher::AddRequest(std::unique_ptr<IRequest> req) {
        std::unique_lock lock(_requestsMutex);
        _requestsToDispatch.emplace(std::move(req));
    }

    std::unique_ptr<IRequest> RatelimitedDispatcher::PopRequest() {
        std::unique_lock lock(_requestsMutex);
        auto req = std::move(_requestsToDispatch.front());
        _requestsToDispatch.pop();
        return std::move(req);
    }

    std::shared_future<void> RatelimitedDispatcher::StartDispatchIfNeeded() {
        // if not valid, or it was completed, start a new future (thread)
        if (!_currentRateLimitDispatch.valid() || _currentRateLimitDispatch.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
            _currentRateLimitDispatch = std::async(std::launch::async, &RatelimitedDispatcher::DispatcherThread, this);
        }
        return _currentRateLimitDispatch;
    }

    std::optional<RatelimitedDispatcher::RetryOptions> RatelimitedDispatcher::RequestFinished(bool success, IRequest* req) const {
        if (onRequestFinished)
            return std::invoke(onRequestFinished, success, req);
        return std::nullopt;
    }

    void RatelimitedDispatcher::AllFinished(std::span<std::unique_ptr<IRequest> const> finishedRequests) {
        if (allFinished) std::invoke(allFinished, finishedRequests);
    }

    void RatelimitedDispatcher::DispatcherThread() {
        while (AnyRequestsToDispatch()) {
            std::vector<std::future<void>> workers;
            // min of define max and field max,
            // max between that and 1 (so we get at least 1),
            // min of that and the amount of reqs we currently have
            int workerCount = std::min<std::size_t>(RequestCountToDispatch(), std::max<std::size_t>(1, std::min(maxConcurrentRequests, WEBUTILS_MAX_CONCURRENCY)));
            for (auto i = 0; i < workerCount; i++) {
                workers.emplace_back(
                    std::async(std::launch::async, &RatelimitedDispatcher::DispatchWorker, this)
                );
            }

            // wait for all workers to finish
            for (auto& d : workers) {
                d.wait();
            }
        }

        std::unique_lock shared_lock(_finishedMutex);
        AllFinished(_finishedRequests);
        shared_lock.unlock();

        std::unique_lock lock(_finishedMutex);
        _finishedRequests.clear();
    }

    void RatelimitedDispatcher::DispatchWorker() {
        // work through backlog
        while (AnyRequestsToDispatch()) {
            // after req complete, wait until the rate limit time has passed
            auto nextRatelimitedRequestTime = std::chrono::high_resolution_clock::now() + rateLimitTime;

            // get req from queue
            auto req = PopRequest();

            // retry options used if response was ratelimited or something
            std::optional<RetryOptions> retryOptions;
            do {
                if (retryOptions.has_value()) std::this_thread::sleep_for(retryOptions->waitTime);

                bool success = downloader.GetInto(req->URL, req->get_TargetResponse());
                retryOptions = RequestFinished(success, req.get());
            } while (retryOptions.has_value());

            std::unique_lock lock(_finishedMutex);
            _finishedRequests.emplace_back(std::move(req));
            lock.unlock();

            // waits until the rate limit has passed, but if retries have happened this could just not sleep
            std::this_thread::sleep_until(nextRatelimitedRequestTime);
        }
    }
}
