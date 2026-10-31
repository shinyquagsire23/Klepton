#pragma once

#ifndef MOD_ID
#define MOD_ID "scotland2"
#endif

#ifndef MOD_VERSION
#define MOD_VERSION "0.1.0"
#endif

#ifdef SL2_KLEPTON
#include <fmt/format.h>
#include <cstdio>
#define SL2_APPLE_LOG(lvl, str, ...) \
  fmt::print(stderr, "[scotland2:{}] " str "\n", lvl __VA_OPT__(, __VA_ARGS__))
#if !defined(SC2_DIAGNOSTICS) || SC2_DIAGNOSTICS
#define LOG_VERBOSE(...) SL2_APPLE_LOG("verbose", __VA_ARGS__)
#define LOG_DEBUG(...) SL2_APPLE_LOG("debug", __VA_ARGS__)
#else
#define LOG_VERBOSE(...) do { if (false) SL2_APPLE_LOG("verbose", __VA_ARGS__); } while (false)
#define LOG_DEBUG(...) do { if (false) SL2_APPLE_LOG("debug", __VA_ARGS__); } while (false)
#endif
#define LOG_INFO(...) SL2_APPLE_LOG("info", __VA_ARGS__)
#define LOG_WARN(...) SL2_APPLE_LOG("warn", __VA_ARGS__)
#define LOG_ERROR(...) SL2_APPLE_LOG("error", __VA_ARGS__)
#define LOG_FATAL(...) SL2_APPLE_LOG("fatal", __VA_ARGS__)
#elif defined(ANDROID)
#include <android/log.h>
#include <fmt/compile.h>
#include <fmt/core.h>

#define SL2_LOG(lvl, str, ...)                                                  \
  do {                                                                          \
    std::string __ss = fmt::format(FMT_COMPILE(str) __VA_OPT__(, __VA_ARGS__)); \
    __android_log_write(lvl, MOD_ID "|v" MOD_VERSION, __ss.c_str());            \
  } while (0)

#define LOG_VERBOSE(str, ...) SL2_LOG(ANDROID_LOG_VERBOSE, str __VA_OPT__(, __VA_ARGS__))
#define LOG_DEBUG(str, ...) SL2_LOG(ANDROID_LOG_DEBUG, str __VA_OPT__(, __VA_ARGS__))
#define LOG_INFO(str, ...) SL2_LOG(ANDROID_LOG_INFO, str __VA_OPT__(, __VA_ARGS__))
#define LOG_WARN(str, ...) SL2_LOG(ANDROID_LOG_WARN, str __VA_OPT__(, __VA_ARGS__))
#define LOG_ERROR(str, ...) SL2_LOG(ANDROID_LOG_ERROR, str __VA_OPT__(, __VA_ARGS__))
#define LOG_FATAL(str, ...) SL2_LOG(ANDROID_LOG_FATAL, str __VA_OPT__(, __VA_ARGS__))
#else

#include <fmt/compile.h>
#include <fmt/core.h>

#define SL2_LOG(lvl, str, ...) \
  fmt::print(FMT_COMPILE(MOD_ID "|v" MOD_VERSION str ": {}: "), lvl __VA_OPT__(, __VA_ARGS__))

#define LOG_VERBOSE(...) SL2_LOG("VERBOSE", __VA_ARGS__)
#define LOG_DEBUG(...) SL2_LOG("DEBUG", __VA_ARGS__)
#define LOG_INFO(...) SL2_LOG("INFO", __VA_ARGS__)
#define LOG_WARN(...) SL2_LOG("WARN", __VA_ARGS__)
#define LOG_ERROR(...) SL2_LOG("ERROR", __VA_ARGS__)
#define LOG_FATAL(...) SL2_LOG("FATAL", __VA_ARGS__)

#endif
