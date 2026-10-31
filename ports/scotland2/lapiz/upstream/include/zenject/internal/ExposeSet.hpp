#pragma once

#include "System/Type.hpp"
#include "Delegate.hpp"

namespace Lapiz::Zenject::Internal {
    class ExposeSet {
        public:
            ExposeSet(Il2CppClass* typeToExpose, std::string locationContractName) : _typeToExpose(typeToExpose), _locationContractName(locationContractName) {}

            __declspec(property(get=get_typeToExpose)) Il2CppClass* TypeToExpose;
            __declspec(property(get=get_locationContractName)) std::string_view LocationContractName;

            auto get_typeToExpose() const { return _typeToExpose; }
            std::string_view get_locationContractName() const { return _locationContractName; }

        private:
            const Il2CppClass* _typeToExpose;
            const std::string _locationContractName;
    };
}
