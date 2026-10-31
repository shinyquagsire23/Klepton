#pragma once

#include "./_config.h"
#include "Data/SongDifficulty.hpp"
#include <vector>

namespace SongDetailsCache {
    struct SONGDETAILS_EXPORT DiffArray {
		public:
            using const_iterator = std::vector<SongDifficulty>::const_iterator;
			[[nodiscard]] const_iterator begin() const noexcept;
			[[nodiscard]] const_iterator end() const noexcept;
			[[nodiscard]] bool empty() const noexcept;
			[[nodiscard]] std::size_t size() const noexcept;
			[[nodiscard]] const SongDifficulty& operator [](std::size_t index) const noexcept;
			/// @brief essentially a bounds checked operator[]
            [[nodiscard]] const SongDifficulty& at(std::size_t index) const noexcept;

            /// @brief deleted because use references
			DiffArray(DiffArray&&) = delete;
			/// @brief deleted because use references
			DiffArray(const DiffArray&) = delete;
        private:
            friend class SongDetails;
            DiffArray() noexcept;
    };
}
