#include "SongArray.hpp"
#include "Utils.hpp"
#include "logging.hpp"
#include "Data/SongDetailsContainer.hpp"

namespace SongDetailsCache {
    SongArray::SongArray() noexcept {};
    SongArray::const_iterator SongArray::begin() const noexcept {
        return SongDetailsContainer::songs->begin();
    }

    SongArray::const_iterator SongArray::end() const noexcept {
        return SongDetailsContainer::songs->end();
    }

    bool SongArray::empty() const noexcept {
        return SongDetailsContainer::songs->empty();
    }

    std::size_t SongArray::size() const noexcept {
        return SongDetailsContainer::songs->size();
    }

    const Song& SongArray::operator [](std::size_t index) const noexcept {
        return SongDetailsContainer::songs->operator[](index);
    }

    const Song& SongArray::at(std::size_t index) const noexcept {
        if (!SongDetailsContainer::songs || index < 0 || index > SongDetailsContainer::songs->size()) return Song::none;
        return operator[](index);
    }

    bool SongArray::get_isDataAvailable() const noexcept { return SongDetailsContainer::get_isDataAvailable(); }

	static constexpr const float hashLookupDivisorInverse = 1.0f / (float)std::numeric_limits<uint32_t>::max();

    bool SongArray::FindByHash(std::string_view hash, const Song*& out) const {
        if (hash.size() != 40) {
            ERROR("Hash size was not 40, this is an invalid hash! hash: {}", hash);
            out = nullptr;
            return false;
        }

        auto songHash = HexUtil::ToSongHash(hash);

        auto sz = size();
		uint32_t searchNeedle = (uint32_t)((float)sz * (float)songHash.c1 * hashLookupDivisorInverse);

		for(uint32_t i = searchNeedle; i < sz; i++) {
			uint32_t songIndex = SongDetailsContainer::hashBytesLUT->operator[](i);
            const auto& hash = SongDetailsContainer::hashBytes->operator[](songIndex);

            if (hash.c1 > songHash.c1)
                break;
            else if (hash.c1 != songHash.c1)
                continue;
            if (hash.c2 == songHash.c2 && hash.c3 == songHash.c3) {
                out = &SongDetailsContainer::songs->operator[](songIndex);
                return true;
            }

        }

		for(uint32_t i = searchNeedle; i-- > 0;) {
			uint32_t songIndex = SongDetailsContainer::hashBytesLUT->operator[](i);
            const auto& hash = SongDetailsContainer::hashBytes->operator[](songIndex);

            if (hash.c1 < songHash.c1)
                break;
            else if (hash.c1 != songHash.c1)
                continue;
            if (hash.c2 == songHash.c2 && hash.c3 == songHash.c3) {
                out = &SongDetailsContainer::songs->operator[](songIndex);
                return true;
            }
        }

        INFO("Song hash '{}' was not found at all in the dataset", hash);
        out = nullptr;
        return false;
    }

    const Song& SongArray::FindByHash(std::string_view hash) const {
        const Song* res = nullptr;
        if (FindByHash(hash, res)) return *res;
        else return Song::none;
    }

    bool SongArray::FindByMapId(const std::string& key, const Song*& out) const {
        return FindByMapId(std::stoul(key, nullptr, 16), out);
    }

    const Song& SongArray::FindByMapId(const std::string& key) const {
        const Song* res;
        if (FindByMapId(key, res)) return *res;
        return Song::none;
    }

    bool SongArray::FindByMapId(uint32_t key, const Song*& out) const {
        auto itr = std::find(SongDetailsContainer::keys->begin(), SongDetailsContainer::keys->end(), key);
        if (itr == SongDetailsContainer::keys->end()) {
            out = nullptr;
            return false;
        }
        out = &SongDetailsContainer::songs->operator[](itr - SongDetailsContainer::keys->begin());
        return true;
    }

    const Song& SongArray::FindByMapId(uint32_t key) const {
        const Song* res;
        if (FindByMapId(key, res)) return *res;
        return Song::none;
    }
}