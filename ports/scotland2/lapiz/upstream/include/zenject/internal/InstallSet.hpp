#pragma once

#include "filters/IInstallFilter.hpp"
#include "System/Type.hpp"

namespace Lapiz::Zenject::Internal {

    class InstallSet {
        public:
            InstallSet(Il2CppClass* installerType, Filters::IInstallFilter* filter, ArrayW<System::Object*> parameters) : _installerType(installerType), _filter(filter), _parameters(parameters) {}

            __declspec(property(get=get_installerType)) Il2CppClass* InstallerType;
            __declspec(property(get=get_filter)) Filters::IInstallFilter* Filter;
            __declspec(property(get=get_parameters)) ArrayW<System::Object*> Parameters;

            auto get_installerType() const { return _installerType; }
            auto get_filter() const { return _filter; }
            auto get_parameters() const { return _parameters; }

        private:
            const Il2CppClass* _installerType;
            const Filters::IInstallFilter* _filter;
            const ArrayW<System::Object*> _parameters;
    };
}
