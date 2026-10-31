#include "Utils/WavRiff.hpp"
#include "logging.hpp"

#include <fstream>

namespace SongCore::Utils {
    /// @brief wav header format based on https://docs.fileformat.com/audio/wav/
    struct WavHeader {
        char riff_id[4];
        int32_t file_size;
        char wav_id[4];
        char format[4];
        int32_t format_data_length;
        int16_t format_type;
        int16_t channel_count;
        int32_t sample_rate;
        int32_t byte_rate; // bitrate / 8, bitrate == sample_rate * bits_per_sample * channel_count
        int16_t block_align; // 1 -> 8bit mono, 2 -> 8bit stereo/16bit mono, 4 -> 16 bit stereo
        int16_t bits_per_sample;
        char data_id[4];
        int32_t data_size;

        operator bool() const {
            if (std::string_view(riff_id, 4) != "RIFF") return false;
            if (std::string_view(wav_id, 4) != "WAVE") return false;
            return true;
        }
    };
    static_assert(sizeof(WavHeader) == 44);

    float GetLengthFromWavRiff(std::filesystem::path const& path) {
        std::ifstream reader(path, std::ios::in | std::ios::binary);

        // parse wav header
        WavHeader header;
        reader.read((char*)&header, sizeof(WavHeader));
        if (!header) {
            WARNING("Could not parse wav header from {}", path.string());
            return -1;
        }

        // calculate sample count from data size & bytes per channel
        int bytes_per_sample = header.bits_per_sample / 8;
        int bytes_per_channels_sample = bytes_per_sample * header.channel_count;
        int sample_count = header.data_size / bytes_per_channels_sample;

        return (double)sample_count / (double)header.sample_rate;
    }
}
