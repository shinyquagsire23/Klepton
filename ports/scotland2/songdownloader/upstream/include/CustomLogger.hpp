#pragma once

#include "beatsaber-hook/shared/utils/utils.h"
#include "paper2_scotland2/shared/logger.hpp"

//#define LOG_INFO(value...)
#define LOG_INFO(str, ...) Paper::Logger::fmtLogTag<Paper::LogLevel::INF>(str, "SongDownloader" __VA_OPT__(, __VA_ARGS__))
//#define LOG_DEBUG(value...)
#define LOG_DEBUG(str, ...) Paper::Logger::fmtLogTag<Paper::LogLevel::DBG>(str, "SongDownloader" __VA_OPT__(, __VA_ARGS__))
//#define LOG_ERROR(value...)
#define LOG_ERROR(str, ...) Paper::Logger::fmtLogTag<Paper::LogLevel::ERR>(str, "SongDownloader" __VA_OPT__(, __VA_ARGS__))
#define LOG_CRITICAL(str, ...) Paper::Logger::fmtLogTag<Paper::LogLevel::CRIT>(str, "SongDownloader" __VA_OPT__(, __VA_ARGS__))
