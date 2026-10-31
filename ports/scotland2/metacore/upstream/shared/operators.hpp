#pragma once

#include <concepts>

#define CREATE_BINARY_OPERATOR(symbol, name)              \
    template <typename T, typename U>                     \
    concept HasOp##name = requires(T a, U b) {            \
        T::op_##name(a, b);                               \
    };                                                    \
    template <typename T, typename U>                     \
    requires HasOp##name<T, U>                            \
    auto operator symbol(const T& lhs, const U& rhs) {    \
        return T::op_##name(lhs, rhs);                    \
    }                                                     \
    template <typename T, typename U>                     \
    requires HasOp##name<T, U> && (!std::is_same_v<T, U>) \
    auto operator symbol(const U& lhs, const T& rhs) {    \
        return T::op_##name(lhs, rhs);                    \
    }

CREATE_BINARY_OPERATOR(==, Equality)
CREATE_BINARY_OPERATOR(!=, Inequality)
CREATE_BINARY_OPERATOR(>, GreaterThan)
CREATE_BINARY_OPERATOR(>=, GreaterThanOrEqual)
CREATE_BINARY_OPERATOR(<, LessThan)
CREATE_BINARY_OPERATOR(<=, LessThanOrEqual)

CREATE_BINARY_OPERATOR(+, Addition)
CREATE_BINARY_OPERATOR(-, Subtraction)
CREATE_BINARY_OPERATOR(*, Multiply)
CREATE_BINARY_OPERATOR(/, Division)

#undef CREATE_BINARY_OPERATOR

#define CREATE_UNARY_OPERATOR(symbol, name) \
    template <typename T>                   \
    concept HasOp##name = requires(T a) {   \
        T::op_##name(a);                    \
    };                                      \
    template <typename T>                   \
    requires HasOp##name<T>                 \
    auto operator symbol(const T& obj) {    \
        return T::op_##name(obj);           \
    }

CREATE_UNARY_OPERATOR(-, UnaryNegation)

#undef CREATE_UNARY_OPERATOR

template <typename T>
concept IsCordlEnum = std::is_integral_v<typename T::__CORDL_BACKING_ENUM_TYPE> &&
                      requires(T t) { static_cast<T>(static_cast<typename T::__CORDL_BACKING_ENUM_TYPE>(t)); };

#define CREATE_BINARY_ENUM_OPERATOR(symbol)                            \
template <class T>                                                     \
requires IsCordlEnum<T>                                                \
T operator symbol(T const& lhs, T const& rhs) {                        \
    return static_cast<T>(                                             \
        static_cast<typename T::__CORDL_BACKING_ENUM_TYPE>(lhs) symbol \
        static_cast<typename T::__CORDL_BACKING_ENUM_TYPE>(rhs)        \
    );                                                                 \
}

CREATE_BINARY_ENUM_OPERATOR(&)
CREATE_BINARY_ENUM_OPERATOR(|)
CREATE_BINARY_ENUM_OPERATOR(^)

#undef CREATE_BINARY_ENUM_OPERATOR

#define CREATE_UNARY_ENUM_OPERATOR(symbol)                             \
template <class T>                                                     \
requires IsCordlEnum<T>                                                \
T operator symbol(T const& obj) {                                      \
    return static_cast<T>(                                             \
        symbol static_cast<typename T::__CORDL_BACKING_ENUM_TYPE>(obj) \
    );                                                                 \
}

CREATE_UNARY_ENUM_OPERATOR(~)

#undef CREATE_UNARY_ENUM_OPERATOR
