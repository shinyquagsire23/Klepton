#pragma once

#include <string>

namespace AirbudsSearch {

class BeatSaverUtils {

    public:
    static BeatSaverUtils& getInstance();

    void init();

    std::string getMP3PreviewDownloadUrl(const std::string& songHash);

    private:
    static constexpr std::string_view BASE_API_URL{"https://api.beatsaver.com/"};

    std::string previewDownloadUrlPrefix_;

    /**
     * Downloading from the default CDN URL is pretty slow. This method will find the region-specific CDN URL.
     * @return
     */
    std::string getPreviewDownloadUrl();
};

}// namespace AirbudsSearch
