#pragma once

#include <map>

namespace MetaCore {
    /// @brief A map that automatically discards the oldest entries past a certain size
    /// @tparam K The key type
    /// @tparam V The value type
    /// @tparam MaxSize The maximum entries to keep, or -1 to never automatically remove entries
    template <class K, class V, int MaxSize = -1>
    struct CacheMap {
        /// @brief Constructor for an empty CacheMap
        CacheMap() : map() {
            head = new Entry();
            tail = new Entry();
            head->next = tail;
            tail->prev = head;
        }

        /// @brief Destructor
        ~CacheMap() {
            while (head->next != tail)
                delete detach(head->next);
            delete head;
            delete tail;
        }

        /// @brief Adds a key/value pair to the map, replacing any existing value of the key and discarding the oldest entry if at MaxSize
        /// @param key The key for the value
        /// @param value The value to add or overwrite
        void push(K key, V value) {
            if (map.contains(key)) {
                map[std::move(key)]->value = value;
                return;
            }
            Entry* entry = new Entry();
            entry->key = std::move(key);
            entry->value = std::move(value);
            push_front(entry);
            map[entry->key] = entry;
            if constexpr (MaxSize > 0) {
                if (size() > MaxSize)
                    drop();
            }
        }

        /// @brief Retrieves the value for a key, adding a default constructed one if not found, and setting it to the newest entry
        /// @param key The key to access
        /// @return A reference to the value
        V& at(K const& key) {
            if (!map.contains(key)) {
                push(key, {});
                return map[key]->value;
            }
            Entry* entry = map[key];
            // move to head on access
            detach(entry);
            push_front(entry);
            return entry->value;
        }

        /// @brief Checks if the map contains a value for a key
        /// @param key The key to check
        /// @return If a value is stored for the key
        bool contains(K const& key) const { return map.contains(key); }

        /// @brief Manually removes the oldest entry from the map
        void drop() {
            Entry* entry = detach(tail->prev);
            map.erase(entry->key);
            delete entry;
        }

        /// @brief Removes all entries from the map
        void clear() {
            while (head->next != tail)
                drop();
        }

        /// @brief Finds the number of entries in the map
        /// @return The number of entries in the map
        size_t size() const {
            size_t size = 0;
            Entry* p = head;
            while (p->next != tail) {
                ++size;
                p = p->next;
            }
            return size;
        }

        V& operator[](K const& key) { return at(key); }

        auto begin() { return map.begin(); }
        auto end() { return map.end(); }
        auto begin() const { return map.begin(); }
        auto end() const { return map.end(); }

       protected:
        struct Entry {
            K key;
            V value;
            Entry* prev;
            Entry* next;
        };

        std::map<K, Entry*> map;
        Entry *head, *tail;
        CacheMap(CacheMap const& rhs);
        CacheMap& operator=(CacheMap const& rhs);

        Entry* detach(Entry* entry) {
            entry->prev->next = entry->next;
            entry->next->prev = entry->prev;
            return entry;
        }

        void push_front(Entry* entry) {
            entry->next = head->next;
            entry->next->prev = entry;
            entry->prev = head;
            head->next = entry;
        }
    };

    /// @brief A map wrapper to easily keep track of values by integer id
    /// @tparam T The value type to store
    template <class T>
    struct IndexMap {
        /// @brief Adds a new value to the map
        /// @param value The value to add
        /// @return The id that can be used to retrieve or remove the value
        int push(T value) {
            this->values.emplace(maxValue, std::move(value));
            return maxValue++;
        }

        /// @brief Retrieves the value for an id
        /// @param id The id of the value
        /// @return The value at the id
        T& at(int id) { return values.at(id); }

        /// @brief Checks if the map contains an value for an id
        /// @param id The id to check
        /// @return If the map contains an value for an id
        bool contains(int id) const { return values.contains(id); }

        /// @brief Removes an id and its value from the map
        /// @param id The id to remove
        void erase(int id) {
            auto itr = values.find(id);
            if (itr != values.end())
                values.erase(itr);
        }

        /// @brief Removes all values from the map
        void clear() { values.clear(); }

        /// @brief Finds the number of values in the map
        /// @return The number of values in the map
        size_t size() const { return values.size(); }

        T& operator[](int id) { return values.at(id); }

        auto begin() { return values.begin(); }
        auto end() { return values.end(); }
        auto begin() const { return values.begin(); }
        auto end() const { return values.end(); }

       protected:
        std::map<int, T> values;
        int maxValue = 0;
    };
}
