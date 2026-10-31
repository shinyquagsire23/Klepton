#include <web-utils/shared/WebUtils.hpp>

#include "BeatSaverUtils.hpp"
#include "Utils.hpp"
#include "Log.hpp"

namespace AirbudsSearch {

BeatSaverUtils& BeatSaverUtils::getInstance() {
    static BeatSaverUtils beatSaverUtils;
    return beatSaverUtils;
}

void BeatSaverUtils::init() {
    previewDownloadUrlPrefix_ = getPreviewDownloadUrl();
    AirbudsSearch::Log.info("BeatSaver preview URL prefix = {}", previewDownloadUrlPrefix_);
}

std::string BeatSaverUtils::getMP3PreviewDownloadUrl(const std::string& songHash) {
    if (previewDownloadUrlPrefix_.empty()) {
        throw std::runtime_error("previewDownloadUrlPrefix_ is empty! Did you forget to call init()?");
    }
    return std::format("{}/{}.mp3", previewDownloadUrlPrefix_, songHash);
}

std::string BeatSaverUtils::getPreviewDownloadUrl() {
    // Arbitrary map ID
    static const std::string MAP_ID{"4a5a2"};
    const std::string url = std::format("{}/maps/id/{}", BeatSaverUtils::BASE_API_URL, MAP_ID);
    AirbudsSearch::Log.info("BeatSaver request: getPreviewDownloadUrl url={} mapId={}", url, MAP_ID);
    const auto response = Get<WebUtils::JsonResponse>(WebUtils::URLOptions(url));
    AirbudsSearch::Log.info(
        "BeatSaver response: getPreviewDownloadUrl http={} curl={} parsed={}",
        response.get_HttpCode(),
        response.get_CurlStatus(),
        response.DataParsedSuccessful());
    const auto& data = response.responseData;
    if (!response.IsSuccessful()) {
        if (data) {
            throw std::runtime_error(std::format("API Error! code = {} data = {}", response.httpCode, Utils::jsonDocumentToString(*data)));
        }
        throw std::runtime_error(std::format("API Error: {}", response.httpCode));
    }
    const rapidjson::Document& document = *data;
    const std::string previewURL = document["versions"].GetArray()[0]["previewURL"].GetString();
    std::vector<std::string> parts;
    for (auto&& part : previewURL | std::views::split('/')) {
        parts.emplace_back(std::string(part.begin(), part.end()));
    }
    std::stringstream stream;
    for (size_t i = 0; i < parts.size() - 1; ++i) {
        stream << parts[i] << "/";
    }
    return stream.str();
}

}
