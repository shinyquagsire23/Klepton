#include "TagsMap.hpp"
#include "Utils.hpp"
#include "logging.hpp"
#include "Data/SongDetailsContainer.hpp"

namespace SongDetailsCache {
    TagsMap::TagsMap() noexcept {};
    TagsMap::const_iterator TagsMap::begin() const noexcept {
        return SongDetailsContainer::tags->begin();
    }

    TagsMap::const_iterator TagsMap::end() const noexcept {
        return SongDetailsContainer::tags->end();
    }

    bool TagsMap::empty() const noexcept {
        return SongDetailsContainer::tags->empty();
    }

    std::size_t TagsMap::size() const noexcept {
        return SongDetailsContainer::tags->size();
    }

    const uint64_t TagsMap::operator [](std::string key) const noexcept {
        auto item = SongDetailsContainer::tags->operator[](key);
        return item;
    }

    const uint64_t TagsMap::at(std::string key) const noexcept {
        if (!SongDetailsContainer::tags || key.empty()) return 0;
        auto item = SongDetailsContainer::tags->find(key);
        if (item == SongDetailsContainer::tags->end()) return 0;
        return item->second;
    }

    bool TagsMap::get_isDataAvailable() const noexcept { return SongDetailsContainer::get_isDataAvailable(); }
}