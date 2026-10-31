#pragma once

#include "./_config.h"
#include <unordered_map>
#include <string>

namespace SongDetailsCache {
    struct SONGDETAILS_EXPORT TagsMap {
		public:
			using const_iterator = std::unordered_map<std::string, uint64_t>::const_iterator;
			[[nodiscard]] const_iterator begin() const noexcept;
			[[nodiscard]] const_iterator end() const noexcept;
			[[nodiscard]] bool empty() const noexcept;
			[[nodiscard]] std::size_t size() const noexcept;
			[[nodiscard]] const uint64_t operator [](std::string index) const noexcept;
			/// @brief essentially a bounds checked operator[]
            [[nodiscard]] const uint64_t at(std::string index) const noexcept;
			[[nodiscard]] bool get_isDataAvailable() const noexcept;

			/// @brief deleted because use references
            TagsMap(TagsMap&&) = delete;
			/// @brief deleted because use references
            TagsMap(const TagsMap&) = delete;
		private:
			friend class SongDetails;
            TagsMap() noexcept;
    };
}
