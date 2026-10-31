#pragma once

#define FLAMINGO_ID "flamingo"
#define FLAMINGO_VERSION "1.1.1"

#ifdef FLAMINGO_HEADER_ONLY
#define FLAMINGO_EXPORT __attribute__((visibility("hidden")))
#else  // FLAMINGO_HEADER_ONLY
#define FLAMINGO_EXPORT __attribute__((visibility("default")))
#endif

#if defined(FLAMINGO_LOG_STANDALONE) || defined(FLAMINGO_HEADER_ONLY)

#ifdef ANDROID
#include <android/log.h>
#include <fmt/core.h>
#include <cstddef>
#include <string>

#define LOGA(lvl, ...)                                                               \
  do {                                                                               \
    std::string __ss = fmt::format(__VA_ARGS__);                                     \
    __android_log_print(lvl, FLAMINGO_ID "|v" FLAMINGO_VERSION, "%s", __ss.c_str()); \
  } while (0)

#ifndef NO_DEBUG_LOGS
#define FLAMINGO_DEBUG(...) LOGA(ANDROID_LOG_DEBUG, __VA_ARGS__)
#define FLAMINGO_ASSERT(...)                                               \
  do {                                                                     \
    if (!(__VA_ARGS__)) FLAMINGO_ABORT("Failed condition: " #__VA_ARGS__); \
  } while (0)
#else
#define FLAMINGO_DEBUG(...)
#define FLAMINGO_ASSERT(...) __builtin_assume(__VA_ARGS__)
#endif

#define FLAMINGO_CRITICAL(...) LOGA(ANDROID_LOG_FATAL, __VA_ARGS__)
#define FLAMINGO_ABORT(...)         \
  do {                              \
    FLAMINGO_CRITICAL(__VA_ARGS__); \
    std::abort();                   \
  } while (0)
#else  // ANDROID
#error "Need logging definitions here, for non-ANDROID, header only support!"
#endif

#else  // FLAMINGO_LOG_STANDALONE

#include <cassert>

#ifdef ANDROID
#include "paper2_scotland2/shared/logger.hpp"

#ifndef NDEBUG
#define FLAMINGO_ASSERT(...) assert(__VA_ARGS__)
#define FLAMINGO_DEBUG(...) Paper::Logger::fmtLog<Paper::LogLevel::DBG>(__VA_ARGS__)
#else
#define FLAMINGO_ASSERT(...) __builtin_assume(__VA_ARGS__)
#define FLAMINGO_DEBUG(...)
#endif

#define FLAMINGO_CRITICAL(...) Paper::Logger::fmtLog<Paper::LogLevel::CRIT>(__VA_ARGS__)
#define FLAMINGO_ABORT(...)         \
  do {                              \
    FLAMINGO_CRITICAL(__VA_ARGS__); \
    Paper::Logger::WaitForFlush();  \
    std::abort();                   \
  } while (0)

#else
#include <cstddef>
#include <cstdio>

#define FLAMINGO_ASSERT(...) assert(__VA_ARGS__)
#define FLAMINGO_DEBUG(...) \
  fmt::print(__VA_ARGS__);  \
  puts("")
#define FLAMINGO_CRITICAL(...) \
  fmt::print(__VA_ARGS__);     \
  puts("")
#define FLAMINGO_ABORT(...) \
  fmt::print(__VA_ARGS__);  \
  puts("");                 \
  std::abort()
#endif

#endif

namespace flamingo::util {

template <class... Ts>
struct overload : Ts... {
  using Ts::operator()...;
};
template <class... Ts>
overload(Ts...) -> overload<Ts...>;

}  // namespace flamingo::util