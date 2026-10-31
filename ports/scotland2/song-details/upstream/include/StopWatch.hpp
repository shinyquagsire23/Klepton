#pragma once

#include <chrono>


struct StopWatch {
    std::chrono::milliseconds _start;
    void Start() { _start = std::chrono::duration_cast<decltype(_start)>(std::chrono::steady_clock::now().time_since_epoch()); }
    void Restart() { Start(); }
    std::size_t EllapsedMilliseconds() const { return (std::chrono::duration_cast<decltype(_start)>(std::chrono::steady_clock::now().time_since_epoch()) - _start).count(); }
};