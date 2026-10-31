#include "DiffArray.hpp"
#include "Data/SongDetailsContainer.hpp"

namespace SongDetailsCache {
    DiffArray::DiffArray() noexcept {}
    DiffArray::const_iterator DiffArray::begin() const noexcept {
        return SongDetailsContainer::difficulties->begin();
    }

	DiffArray::const_iterator DiffArray::end() const noexcept {
        return SongDetailsContainer::difficulties->end();
    }

	bool DiffArray::empty() const noexcept {
        return SongDetailsContainer::difficulties->empty();
    }

	std::size_t DiffArray::size() const noexcept {
        return SongDetailsContainer::difficulties->size();
    }

	const SongDifficulty& DiffArray::operator [](std::size_t index) const noexcept {
        return SongDetailsContainer::difficulties->operator [](index);
    }

    const SongDifficulty& DiffArray::at(std::size_t index) const noexcept {
        if (!SongDetailsContainer::difficulties || index < 0 || index > SongDetailsContainer::difficulties->size()) return SongDifficulty::none;
        return operator[](index);
    }
}