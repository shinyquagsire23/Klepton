#pragma once
#include "config-utils/shared/config-utils.hpp"

DECLARE_CONFIG(ModConfig) {

    CONFIG_VALUE(Service, std::string, "Service", "BeatSaver", "Selected Service to get Information from!");
    CONFIG_VALUE(ListType_BeatSaver, std::string, "List_BeatSaver", "Search", "What Type of Information to display!");
    CONFIG_VALUE(ListType_ScoreSaber, std::string, "ListType_ScoreSaber", "Top Trending", "What Type of Information to display!");
    CONFIG_VALUE(AutoMapper, std::string, "AutoMapper", "", "Show auto-generated Beatmaps");
    CONFIG_VALUE(SortOrder, std::string, "Sort Order", "Relevance", "How results should be sorted");
    CONFIG_VALUE(NE, std::string, "NE", "", "If NoodleExtensions maps should be included in search results");
    CONFIG_VALUE(ME, std::string, "ME", "", "If MappingExtensions maps should be included in search results");
    CONFIG_VALUE(Chroma, std::string, "Chroma", "", "If Chroma maps should be included in search results");
    CONFIG_VALUE(Ranked, std::string, "Ranked_BeatSaver", "", "If Ranked maps should be included in search results");

    CONFIG_VALUE(BookmarkUsername, std::string, "BookmarkUsername", "");
};
