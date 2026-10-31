#include "ThreadPool.hpp"

namespace AirbudsSearch {

ThreadPool::ThreadPool(size_t maxThreadCount) : maxThreadCount_(maxThreadCount) {}

void ThreadPool::submit(const std::function<void()>& task) {
    std::unique_lock lock(mutex_);

    // Wait for a thread to become available
    conditionVariable_.wait(lock, [this]() {
        return jobs_.size() < maxThreadCount_;
    });

    // Create a job
    static size_t id = 0;
    Job job{id++, task};
    std::thread thread([this, job]() {
        job.task_();
        onJobFinished(job);
    });
    thread.detach();
    jobs_.push_back(job);
}

void ThreadPool::wait() {
    std::unique_lock lock(mutex_);
    conditionVariable_.wait(lock, [this]() {
        return jobs_.empty();
    });
}

void ThreadPool::onJobFinished(const Job& job) {
    std::unique_lock lock(mutex_);
    jobs_.erase(std::remove(jobs_.begin(), jobs_.end(), job), jobs_.end());
    conditionVariable_.notify_all();
}

void ThreadPool::setMaxThreadCount(const size_t maxThreadCount) {
    maxThreadCount_ = maxThreadCount;
}

} // namespace AirbudsSearch
