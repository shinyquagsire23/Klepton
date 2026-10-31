#pragma once
#include <string_view>
#include "paper2_scotland2/shared/logger.hpp"

#define INFO(str, ...) Paper::Logger::fmtLogTag<Paper::LogLevel::INF>(str, "SongDetails" __VA_OPT__(, __VA_ARGS__))
#define ERROR(str, ...) Paper::Logger::fmtLogTag<Paper::LogLevel::ERR>(str, "SongDetails" __VA_OPT__(, __VA_ARGS__))
#define CRITICAL(str, ...) Paper::Logger::fmtLogTag<Paper::LogLevel::ERR>(str, "SongDetails" __VA_OPT__(, __VA_ARGS__))
#define DEBUG(str, ...) Paper::Logger::fmtLogTag<Paper::LogLevel::DBG>(str, "SongDetails" __VA_OPT__(, __VA_ARGS__))
#define WARNING(str, ...) Paper::Logger::fmtLogTag<Paper::LogLevel::WRN>(str, "SongDetails" __VA_OPT__(, __VA_ARGS__))
