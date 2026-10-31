#pragma once
#include <string>

#include "beatsaber-hook/shared/config/rapidjson-utils.hpp"

namespace WebUtils {

    std::optional<rapidjson::Document> GetJSON(std::string url);

    long Get(std::string url, std::string& val);

    long Get(std::string url, long timeout, std::string& val);

    void GetAsync(std::string url, std::function<void(long, std::string)> finished, std::function<void(float)> progressUpdate = nullptr);

    void GetAsync(std::string url, long timeout, std::function<void(long, std::string)> finished, std::function<void(float)> progressUpdate = nullptr);

    void GetJSONAsync(std::string url, std::function<void(long, bool, rapidjson::Document&)> finished);

}