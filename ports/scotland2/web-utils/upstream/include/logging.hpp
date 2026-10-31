#pragma once

#include <android/log.h>
#include <fmt/compile.h>
#include <fmt/core.h>

#define DO_LOG(lvl, str, ...) do {                                             \
    std::string __ss = fmt::format(FMT_COMPILE(str) __VA_OPT__(, __VA_ARGS__)); \
    __android_log_write(lvl, MOD_ID "|v" VERSION, __ss.c_str());            \
} while (0)

#define VERBOSE(str, ...) DO_LOG(ANDROID_LOG_VERBOSE, str __VA_OPT__(, __VA_ARGS__))
#define DEBUG(str, ...) DO_LOG(ANDROID_LOG_DEBUG, str __VA_OPT__(, __VA_ARGS__))
#define INFO(str, ...) DO_LOG(ANDROID_LOG_INFO, str __VA_OPT__(, __VA_ARGS__))
#define WARN(str, ...) DO_LOG(ANDROID_LOG_WARN, str __VA_OPT__(, __VA_ARGS__))
#define ERROR(str, ...) DO_LOG(ANDROID_LOG_ERROR, str __VA_OPT__(, __VA_ARGS__))
#define FATAL(str, ...) DO_LOG(ANDROID_LOG_FATAL, str __VA_OPT__(, __VA_ARGS__))
