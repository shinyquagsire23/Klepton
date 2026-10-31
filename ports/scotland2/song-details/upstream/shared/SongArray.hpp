#pragma once

#include "./_config.h"
#include "Data/Song.hpp"

namespace SongDetailsCache {
    struct SONGDETAILS_EXPORT SongArray {
		public:
			using const_iterator = std::vector<Song>::const_iterator;
			[[nodiscard]] const_iterator begin() const noexcept;
			[[nodiscard]] const_iterator end() const noexcept;
			[[nodiscard]] bool empty() const noexcept;
			[[nodiscard]] std::size_t size() const noexcept;
			[[nodiscard]] const Song& operator [](std::size_t index) const noexcept;
			/// @brief essentially a bounds checked operator[]
            [[nodiscard]] const Song& at(std::size_t index) const noexcept;
			[[nodiscard]] bool get_isDataAvailable() const noexcept;

			/// @brief Gets a song using its Map Hash
			/// @param hash hexadecimal Map Hash, captialization does not matter
			/// @param song the song - Will be a random song if not found, make sure to check the return value of the method!
			/// @return True if the song was found, false otherwise
			bool FindByHash(std::string_view hash, const Song*& out) const;
			/// @brief Gets a song using its Map Hash
			/// @param hash hexadecimal Map Hash, captialization does not matter
			/// @return Found song, otherwise returns Song::none
			const Song& FindByHash(std::string_view hash) const;

			/// @brief Gets a song using its hexadecimal Map ID (Some times called Map Key)
			/// @param key hexadecimal Map ID, captialization does not matter
			/// @param song the song - Will be a random song if not found, make sure to check the return value of the method!
			/// @return True if the song was found, false otherwise
			bool FindByMapId(const std::string& key, const Song*& out) const;
			/// @brief Gets a song using its hexadecimal Map ID (Some times called Map Key)
			/// @param key hexadecimal Map ID, captialization does not matter
			/// @return Found song, otherwise returns Song::none
			const Song& FindByMapId(const std::string& key) const;

			/// @brief Gets a song using its Map ID
			/// @param key Map ID
			/// @param song the song - Will be a random song if not found, make sure to check the return value of the method!
			/// @return True if the song was found, false otherwise
			bool FindByMapId(uint32_t key, const Song*& out) const;
			/// @brief Gets a song using its Map ID
			/// @param key Map ID
			/// @return Found song, otherwise returns Song::none
			const Song& FindByMapId(uint32_t key) const;

			/// @brief deleted because use references
			SongArray(SongArray&&) = delete;
			/// @brief deleted because use references
			SongArray(const SongArray&) = delete;
		private:
			friend class SongDetails;
			SongArray() noexcept;
    };
}
