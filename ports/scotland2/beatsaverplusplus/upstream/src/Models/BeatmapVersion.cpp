#include "Models/BeatmapVersion.hpp"
#include "BeatSaver.hpp"
#include "Utils.hpp"

namespace BeatSaver::Models {
    std::optional<std::filesystem::path> BeatmapVersion::DownloadBeatmap(Beatmap const& beatmap, std::function<void(float)> progressReport) const {
        return BeatSaver::API::DownloadBeatmap({beatmap, *this}, std::forward<std::function<void(float)>>(progressReport));
    }

    std::future<std::optional<std::filesystem::path>> BeatmapVersion::DownloadBeatmapAsync(Beatmap const& beatmap, std::function<void(float)> progressReport) const {
        return std::async(std::launch::any, BeatSaver::API::DownloadBeatmap, BeatSaver::API::BeatmapDownloadInfo{beatmap, *this}, std::forward<std::function<void(float)>>(progressReport));
    }

    void BeatmapVersion::DownloadBeatmapAsync(Beatmap const& beatmap, std::function<void(std::optional<std::filesystem::path>)> onFinished, std::function<void(float)> progressReport) const {
        return BeatSaver::API::DownloadBeatmapAsync({beatmap, *this}, onFinished, std::forward<std::function<void(float)>>(progressReport));
    }

    std::optional<std::vector<uint8_t>> BeatmapVersion::GetCoverImage() const {
        return Utils::GetData(CoverURL);
    }

    void BeatmapVersion::GetCoverImageAsync(std::function<void(std::optional<std::vector<uint8_t>>)> onFinished) const {
        if (!onFinished) return;

        std::thread([](std::string coverURL, std::function<void(std::optional<std::vector<uint8_t>>)> onFinished){
            onFinished(Utils::GetData(coverURL));
        }, std::forward<std::string>(CoverURL), std::forward<std::function<void(std::optional<std::vector<uint8_t>>)>>(onFinished)).detach();
    }

    std::future<std::optional<std::vector<uint8_t>>> BeatmapVersion::GetCoverImageAsync() const {
        return std::async(std::launch::any, &Utils::GetData, CoverURL);
    }

    std::optional<std::vector<uint8_t>> BeatmapVersion::GetPreview() const {
        return Utils::GetData(PreviewURL);
    }

    void BeatmapVersion::GetPreviewAsync(std::function<void(std::optional<std::vector<uint8_t>>)> onFinished) const {
        if (!onFinished) return;

        std::thread([](std::string previewURL, std::function<void(std::optional<std::vector<uint8_t>>)> onFinished){
            onFinished(Utils::GetData(previewURL));
        }, std::forward<std::string>(PreviewURL), std::forward<std::function<void(std::optional<std::vector<uint8_t>>)>>(onFinished)).detach();
    }

    std::future<std::optional<std::vector<uint8_t>>> BeatmapVersion::GetPreviewAsync() const {
        return std::async(std::launch::any, &Utils::GetData, PreviewURL);
    }
}
