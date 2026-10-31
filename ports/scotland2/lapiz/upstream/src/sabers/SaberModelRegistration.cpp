#include "sabers/SaberModelRegistration.hpp"
#include "utilities/logging.hpp"
#include "utilities/typeutil.hpp"

DEFINE_TYPE(Lapiz::Sabers, SaberModelRegistrationWrapper);

namespace Lapiz::Sabers {
        SaberModelRegistration::SaberModelRegistration(const Il2CppClass* type, int priority) {
            if (!type || !TypeUtil::hasAncestor<GlobalNamespace::SaberModelController*>(type)) {
                throw std::invalid_argument(fmt::format("Type {}::{} does not have SaberModelController as ancestor!", type->namespaze, type->name));
            }

            _priority = priority;
            _leftType = _rightType = type;
        }

        SaberModelRegistration::SaberModelRegistration(const Il2CppClass* leftType, const Il2CppClass* rightType, int priority) {
            if (!leftType || !TypeUtil::hasAncestor<GlobalNamespace::SaberModelController*>(leftType)) {
                throw std::invalid_argument(fmt::format("Type {}::{} does not have SaberModelController as ancestor!", leftType->namespaze, leftType->name));
            }

            if (!rightType || !TypeUtil::hasAncestor<GlobalNamespace::SaberModelController*>(rightType)) {
                throw std::invalid_argument(fmt::format("Type {}::{} does not have SaberModelController as ancestor!", rightType->namespaze, rightType->name));
            }

            _priority = priority;
            _leftType = leftType;
            _rightType = rightType;
        }

        SaberModelRegistration::SaberModelRegistration(GlobalNamespace::SaberModelController* prefab, int priority) {
            if (!prefab || !prefab->m_CachedPtr.m_value) {
                throw std::invalid_argument("Prefab was not valid!");
            }

            _priority = priority;
            _leftTemplate = prefab;
            _rightTemplate = prefab;
        }

        SaberModelRegistration::SaberModelRegistration(GlobalNamespace::SaberModelController* leftModelPrefab, GlobalNamespace::SaberModelController* rightModelPrefab, int priority) {
            if (!leftModelPrefab || !leftModelPrefab->m_CachedPtr.m_value) {
                throw std::invalid_argument("Left prefab was not valid!");
            }
            
            if (!rightModelPrefab || !rightModelPrefab->m_CachedPtr.m_value) {
                throw std::invalid_argument("Right prefab was not valid!");
            }

            _priority = priority;
            _leftTemplate = leftModelPrefab;
            _rightTemplate = rightModelPrefab;

        }

        SaberModelRegistration::SaberModelRegistration(std::function<GlobalNamespace::SaberModelController*(void)> instruction, int priority) {
            if (!instruction) {
                throw std::invalid_argument("Instruction was not valid!");
            }

            _priority = priority;
            _leftInstruction = _rightInstruction = instruction;
        }

        SaberModelRegistration::SaberModelRegistration(std::function<GlobalNamespace::SaberModelController*(void)> leftInstruction, std::function<GlobalNamespace::SaberModelController*(void)> rightInstruction, int priority) {
            if (!leftInstruction) {
                throw std::invalid_argument("Left instruction was not valid!");
            }

            if (!rightInstruction) {
                throw std::invalid_argument("Right instruction was not valid!");
            }

            _priority = priority;
            _leftInstruction = leftInstruction;
            _rightInstruction = rightInstruction;
        }

        SaberModelRegistrationWrapper* SaberModelRegistrationWrapper::Make(std::shared_ptr<SaberModelRegistration> registration) {
            auto self = SaberModelRegistrationWrapper::New_ctor();
            self->_registration = registration;
            return self;
        }

}
