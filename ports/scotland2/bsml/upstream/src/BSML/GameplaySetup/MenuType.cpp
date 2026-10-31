#include "BSML/GameplaySetup/MenuType.hpp"

namespace BSML {
    bool operator!(const MenuType& type) {
        return type != MenuType::None;
    }

    MenuType operator |(const MenuType& lhs, const MenuType& rhs) {
        return static_cast<MenuType>(static_cast<int>(lhs) | static_cast<int>(rhs));
    }

    MenuType& operator |=(MenuType& lhs, const MenuType& rhs) {
        lhs = lhs | rhs;
        return lhs;
    } 

    MenuType operator &(const MenuType& lhs, const MenuType& rhs) {
        return static_cast<MenuType>(static_cast<int>(lhs) & static_cast<int>(rhs));
    } 

    MenuType& operator &=(MenuType& lhs, const MenuType& rhs) {
        lhs = lhs & rhs;
        return lhs;
    } 
}