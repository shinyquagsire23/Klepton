#pragma once

#include "../_config.h"
#include "custom-types/shared/macros.hpp"
#include "GlobalNamespace/SaberModelController.hpp"

namespace Lapiz::Sabers {
    class LAPIZ_EXPORT SaberModelRegistration {
        public:
            template<typename T>
            requires(std::is_convertible_v<T, GlobalNamespace::SaberModelController*>)
            static std::shared_ptr<SaberModelRegistration> Create(int priority = 0) {
                return std::make_shared<SaberModelRegistration>(classof(T), priority);
            }

            SaberModelRegistration(const Il2CppClass* type, int priority = 0);
            SaberModelRegistration(const Il2CppClass* leftType, const Il2CppClass* rightType, int priority = 0);
            SaberModelRegistration(GlobalNamespace::SaberModelController* prefab, int priority = 0);
            SaberModelRegistration(GlobalNamespace::SaberModelController* leftModelPrefab, GlobalNamespace::SaberModelController* rightModelPrefab, int priority = 0);
            SaberModelRegistration(std::function<GlobalNamespace::SaberModelController*(void)> instruction, int priority = 0);
            SaberModelRegistration(std::function<GlobalNamespace::SaberModelController*(void)> leftInstruction, std::function<GlobalNamespace::SaberModelController*(void)> rightInstruction, int priority = 0);

            bool operator >=(const SaberModelRegistration& other) const { return _priority >= other._priority; }
            bool operator >(const SaberModelRegistration& other) const { return _priority > other._priority; }
            bool operator <=(const SaberModelRegistration& other) const { return _priority <= other._priority; }
            bool operator <(const SaberModelRegistration& other) const { return _priority < other._priority; }

            auto get_leftType() const { return _leftType; }
            auto get_rightType() const { return _rightType; }
            auto get_leftTemplate() const { return _leftTemplate; }
            auto get_rightTemplate() const { return _rightTemplate; }
            auto get_leftInstruction() const { return _leftInstruction; }
            auto get_rightInstruction() const { return _rightInstruction; }

        private:
            friend class SaberModelProvider;
            int _priority;

            const Il2CppClass* _leftType = nullptr;
            const Il2CppClass* _rightType = nullptr;

            SafePtrUnity<GlobalNamespace::SaberModelController> _leftTemplate = nullptr;
            SafePtrUnity<GlobalNamespace::SaberModelController> _rightTemplate = nullptr;

            std::function<GlobalNamespace::SaberModelController*(void)> _leftInstruction = nullptr;
            std::function<GlobalNamespace::SaberModelController*(void)> _rightInstruction = nullptr;
    };
}

DECLARE_CLASS_CODEGEN(Lapiz::Sabers, SaberModelRegistrationWrapper, System::Object) {
    public:
        static SaberModelRegistrationWrapper* Make(std::shared_ptr<SaberModelRegistration> registration);
        DECLARE_DEFAULT_CTOR();
        DECLARE_SIMPLE_DTOR();
    private:
        friend class SaberModelProvider;
        std::shared_ptr<SaberModelRegistration> _registration;
};