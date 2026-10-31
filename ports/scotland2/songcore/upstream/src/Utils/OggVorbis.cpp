#include "Utils/OggVorbis.hpp"
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <cstdint>
#include <span>

#include "logging.hpp"
#include "Utils/File.hpp"

#define OFFSET (8 + 2 + 4)

const uint8_t VORBIS[] = { 0x76, 0x6F, 0x72, 0x62, 0x69, 0x73 };
const uint8_t OGG[] = { 0x4F, 0x67, 0x67, 0x53, 0x00, 0x04 };

namespace SongCore::Utils {
    /// @brief utility method to get the index of a byte in a span of bytes
    /// @param bytes the span to find in
    /// @param search the byte to look for
    /// @return -1 for not found, index otherwise
    int IndexOf(std::span<const uint8_t> bytes, uint8_t search) {
        auto itr = std::find(bytes.begin(), bytes.end(), search);
        if (itr == bytes.end()) return -1;
        return std::distance(bytes.begin(), itr);
    }

    /// @brief utility method to find bytes within a reader, modifies the reader to be at a different placement
    /// @param reader the reader to read bytes from
    /// @param bytes the bytes to search for
    /// @param searchLength the maximum length allowed to be advanced on the reader
    bool FindBytes(std::ifstream& reader, std::span<const uint8_t> searchBytes, int searchLength) {
        if (searchBytes.size() < 6) throw std::runtime_error("the bytes to search for need to have at least a length of 6!");

        for (int i = 0; i < searchLength; i++) {
            // get byte
            uint8_t b;
            reader.read((char*)&b, sizeof(uint8_t));

            // if the first byte doesn't match, we can already just continue
            if (b != searchBytes[0]) continue;

            // read the next searchBytes.size() - 1 bytes
            std::vector<uint8_t> by(searchBytes.size() - 1);
            reader.read((char*)by.data(), by.size() * sizeof(uint8_t));

            // compare the read bytes with the rest of the bytes
            if (by[0] == searchBytes[1]
                && by[1] == searchBytes[2]
                && by[2] == searchBytes[3]
                && by[3] == searchBytes[4]
                && (by[4] & searchBytes[5]) == searchBytes[5]) {
                return true;
            }

            // get the index of the first search byte in the next read bytes
            int idx = IndexOf(by, searchBytes[0]);

            if (idx != -1) {
                reader.seekg((size_t)reader.tellg() + (idx - (by.size())), std::ios::beg);
                i += idx;
            } else {
                i += by.size();
            }
        }

        // we got through the entire searchLength without finding the bytes we were looking for
        return false;
    }

    float GetLengthFromOggVorbis(std::filesystem::path path) {
        std::ifstream reader(path, std::ios::in | std::ios::binary | std::ios::ate);
        size_t fileLen = reader.tellg();

        int32_t rate = -1;
        int64_t lastSample = -1;

        reader.seekg(24, std::ios::beg);

        auto foundVorbis = FindBytes(reader, VORBIS, 256);
        if (foundVorbis) {
            reader.seekg((size_t)reader.tellg() + 5, std::ios::beg);
            reader.read((char*)&rate, sizeof(int32_t));
        } else {
            WARNING("Could not find rate for {}", path.string());
            return -1;
        }

        /**
         * This code will search in blocks from the end of the file to find the last sample
         * it reads in blocks of size SEEK_BLOCK_SIZE
         */
        static constexpr int SEEK_BLOCK_SIZE = 6144;
        static constexpr int SEEK_TRIES = 10;

        for (int i = 0; i < SEEK_TRIES; i++) {
            // calculate the position from the end to start reading
            int64_t seekPos = (i + 1) * SEEK_BLOCK_SIZE;
            auto overshoot = std::max((int64_t)(seekPos - fileLen), 0l);
            if (overshoot >= SEEK_BLOCK_SIZE) break;

            // set the reader at end - seekPos + overshoot
            reader.seekg(overshoot - seekPos, std::ios::seekdir::end);

            // check to find the OGG bytes
            auto foundOggS = FindBytes(reader, OGG, SEEK_BLOCK_SIZE - overshoot);
            if (foundOggS) {
                reader.read((char*)&lastSample, sizeof(int64_t));
                break;
            }
        }

        if (lastSample == -1) {
            WARNING("Could not find last sample for {}", path.string());
            return -1;
        }

        return (float) lastSample / (float) rate;
    }
}
