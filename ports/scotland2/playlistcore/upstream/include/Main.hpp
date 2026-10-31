#pragma once

#include "beatsaber-hook/shared/utils/logging.hpp"

static constexpr auto logger = Paper::ConstLoggerContext(MOD_ID);

// #define LOG_INFO(value...)
#define LOG_INFO(value...) logger.info(value)
#define LOG_DEBUG(value...)
// #define LOG_DEBUG(value...) logger.debug(value)
// #define LOG_ERROR(value...)
#define LOG_ERROR(value...) logger.error(value)

#define LOWER(string) std::transform(string.begin(), string.end(), string.begin(), tolower)

#define CustomLevelPackPrefixID "custom_levelPack_"
#define CustomLevelsPackID CustomLevelPackPrefixID "CustomLevels"
#define CustomWIPLevelsPackID CustomLevelPackPrefixID "CustomWIPLevels"

std::string GetPlaylistsPath();
std::string GetConfigPath();
std::string GetCoversPath();
