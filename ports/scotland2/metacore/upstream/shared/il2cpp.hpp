#pragma once

#include <fmt/format.h>

#include "System/Collections/Generic/Dictionary_2.hpp"
#include "System/Collections/Generic/InsertionBehavior.hpp"
#include "beatsaber-hook/shared/utils/typedefs.h"

// experimental

template <typename T, il2cpp_array_size_t N = 0>
struct ConstArray {
    ConstArray() = default;
    ConstArray(ConstArray const&) = default;
    ConstArray(ConstArray&&) = default;

    template <typename... Ts>
    constexpr ConstArray(T in, Ts... ins) : data{in, ins...} {}

    void init() const noexcept {
        if (!this->klass)
            const_cast<ConstArray<T, N>*>(this)->klass = classof(Array<T>*);
    }

    constexpr Array<T>* to_array() {
        init();
        return reinterpret_cast<Array<T>*>(&klass);
    }
    constexpr Array<T>* to_array() const {
        init();
        return reinterpret_cast<Array<T>*>(&klass);
    }

    constexpr operator ArrayW<T>() { return to_array(); }
    constexpr operator ArrayW<T>() const { return to_array(); }

   private:
    Il2CppClass* klass = nullptr;
    MonitorData* monitor = nullptr;
    Il2CppArrayBounds* bounds = nullptr;
    il2cpp_array_size_t max_length = N;
    T data[N] = {};
};

template <typename T, typename... Ts>
ConstArray(T in, Ts... ins) -> ConstArray<T, sizeof...(Ts) + 1>;

template <typename TKey, typename TValue>
struct DictionaryW {
    using Wrapped = System::Collections::Generic::Dictionary_2<TKey, TValue>;

    constexpr DictionaryW(void* ptr) noexcept : ptr(static_cast<Wrapped*>(ptr)) {}
    constexpr DictionaryW(Wrapped* ptr) noexcept : ptr(ptr) {}
    constexpr DictionaryW(std::nullptr_t npt) noexcept : ptr(npt) {}
    constexpr DictionaryW() noexcept : ptr(nullptr) {}

    constexpr DictionaryW(DictionaryW const&) noexcept = default;
    constexpr DictionaryW(DictionaryW&&) noexcept = default;

    DictionaryW(std::map<TKey, TValue> map) : ptr(New()) {
        for (auto const& [key, value] : map)
            insert(key, value);
    };
    DictionaryW(std::unordered_map<TKey, TValue> map) : ptr(New()) {
        for (auto const& [key, value] : map)
            insert(key, value);
    };

    static inline DictionaryW New() { return Wrapped::New_ctor(); }

    int size() const noexcept { return ptr->_count - ptr->_freeCount; }

    bool empty() const noexcept { return size() == 0; }

    void clear() {
        if (ptr->_count == 0)
            return;
        ptr->Clear();
    }

    void insert(TKey const& key, TValue const& value) {
        ptr->TryInsert(key, value, System::Collections::Generic::InsertionBehavior::OverwriteExisting);
    }

    bool try_insert(TKey const& key, TValue const& value) { return ptr->TryAdd(key, value); }

    bool contains(TKey const& key) const { return ptr->ContainsKey(key); }

    void erase(TKey const& key) { ptr->Remove(key); }

    TValue& at(TKey const& key) {
        int num = ptr->FindEntry(key);
        if (num < 0)
            throw std::runtime_error("Could not find key in DictionaryW!");
        return ptr->_entries[num].value;
    }

    TValue& operator[](TKey const& key) {
        int num = ptr->FindEntry(key);
        if (num >= 0)
            return ptr->_entries[num].value;
        ptr->TryInsert(key, {}, System::Collections::Generic::InsertionBehavior::OverwriteExisting);
        return at(key);
    }

    template <typename T>
    struct Iterator {
        constexpr Iterator(Wrapped* ptr, int index) : ptr(ptr), version(ptr->_version), index(index) {
            if (index < ptr->_count && ptr->_entries[index].hashCode < 0)
                ++(*this);
        }
        constexpr Iterator(Wrapped* ptr) : Iterator(ptr, 0) {}
        constexpr Iterator(DictionaryW<TKey, TValue> dict) : Iterator((Wrapped*) dict.convert()) {}

        constexpr Iterator(Iterator const&) = default;
        constexpr Iterator(Iterator&&) = default;

        using value_type = T::value_type;

        void check_version() const {
            if (version != ptr->_version)
                throw std::runtime_error("DictionaryW was modified during iteration!");
        }

        value_type operator*() const {
            check_version();
            auto& entry = ptr->_entries[index];
            if constexpr (std::is_same_v<T, void>)
                return std::make_pair(entry.key, entry.value);
            else
                return T::get(entry);
        }
        auto* operator->() const { return &(operator*()); }

        Iterator& operator++() {
            check_version();
            while (++index < ptr->_count && ptr->_entries[index].hashCode < 0)
                continue;
            return *this;
        }
        Iterator operator++(int) {
            auto tmp = *this;
            ++(*this);
            return tmp;
        }

        bool operator==(Iterator const& other) const = default;

       private:
        int version;
        int index;
        Wrapped* ptr;
    };

    struct Entries {
        using value_type = std::pair<TKey, TValue>;

       private:
        friend struct Iterator<Entries>;
        static inline value_type get(Wrapped::Entry& entry) { return std::make_pair(entry.key, entry.value); }
    };

    struct Keys {
        constexpr Keys(Wrapped* ptr) noexcept : ptr(ptr) {}
        constexpr Keys(DictionaryW<TKey, TValue> dict) noexcept : Keys((Wrapped*) dict.convert()) {}

        constexpr Keys(Keys const&) noexcept = default;
        constexpr Keys(Keys&&) noexcept = default;

        bool operator==(Keys const& other) const = default;

        auto begin() { return Iterator<Keys>(ptr); }
        auto end() { return Iterator<Keys>(ptr, ptr->_count); }

        using value_type = TKey;

       private:
        friend struct Iterator<Keys>;
        static inline value_type get(Wrapped::Entry& entry) { return entry.key; }

        Wrapped* ptr;
    };

    struct Values {
        Values(Wrapped* ptr) : ptr(ptr) {}
        Values(DictionaryW<TKey, TValue> dict) : Values((Wrapped*) dict.convert()) {}

        Values(Values const&) = default;
        Values(Values&&) = default;

        bool operator==(Values const& other) const = default;

        auto begin() { return Iterator<Values>(ptr); }
        auto end() { return Iterator<Values>(ptr, ptr->_count); }

        using value_type = TValue&;

       private:
        friend struct Iterator<Values>;
        static inline value_type get(Wrapped::Entry& entry) { return entry.value; }

        Wrapped* ptr;
    };

    Keys keys() noexcept { return Keys(ptr); }

    Values values() noexcept { return Values(ptr); }

    auto begin() { return Iterator<Entries>(ptr); }
    auto end() { return Iterator<Entries>(ptr, ptr->_count); }

    operator Wrapped*() noexcept { return ptr; }
    Wrapped operator->() noexcept { return ptr; }

    void* convert() const noexcept { return ptr; }

   private:
    Wrapped* ptr;
};

MARK_GEN_REF_T(DictionaryW);
static_assert(il2cpp_utils::has_il2cpp_conversion<DictionaryW<int, int>>);

template <typename TKey, typename TValue>
struct BS_HOOKS_HIDDEN ::il2cpp_utils::il2cpp_type_check::need_box<DictionaryW<TKey, TValue>> {
    constexpr static bool value = false;
};

template <typename TKey, typename TValue>
struct BS_HOOKS_HIDDEN ::il2cpp_utils::il2cpp_type_check::il2cpp_no_arg_class<DictionaryW<TKey, TValue>> {
    static inline Il2CppClass* get() {
        auto klass = ::il2cpp_utils::il2cpp_type_check::il2cpp_no_arg_class<typename DictionaryW<TKey, TValue>::Wrapped*>::get();
        return klass;
    }
};

template <typename TKey, typename TValue, typename Char>
struct fmt::is_range<DictionaryW<TKey, TValue>, Char> {
    static constexpr bool const value = false;
};

template <typename TKey, typename TValue>
auto format_as(DictionaryW<TKey, TValue> dict) {
    if (!dict)
        return std::string("null");
    return fmt::format("{{{}}}", fmt::join(dict.begin(), dict.end(), ", "));
}
