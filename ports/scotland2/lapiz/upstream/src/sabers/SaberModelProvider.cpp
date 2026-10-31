#include "sabers/SaberModelProvider.hpp"

#include "utilities/saberutil.hpp"
#include "GlobalNamespace/Saber.hpp"
#include "GlobalNamespace/SaberTypeObject.hpp"
#include "GlobalNamespace/SaberType.hpp"
#include "UnityEngine/GameObject.hpp"

DEFINE_TYPE(Lapiz::Sabers, SaberModelProvider);

using namespace GlobalNamespace;

namespace Lapiz::Sabers {
    SaberModelProvider* SaberModelProvider::instance;
    SaberModelProvider* SaberModelProvider::get_instance() {
        return instance;
    }

    void SaberModelProvider::ctor(Zenject::DiContainer* container, GlobalNamespace::SaberManager* saberManager, ListW<SaberModelRegistrationWrapper*> saberModelRegistrations) {
        INVOKE_CTOR();
        instance = this;
        _earlyInittingGlowColors = System::Collections::Generic::HashSet_1<GlobalNamespace::SetSaberGlowColor*>::New_ctor();
        _earlyInittingFakeGlowColors = System::Collections::Generic::HashSet_1<GlobalNamespace::SetSaberFakeGlowColor*>::New_ctor();

        _container = container;
        _saberManager = saberManager;

        _localLeftContainer = saberManager->get_leftSaber()->GetComponent<SaberModelContainer*>();
        _localRightContainer = saberManager->get_rightSaber()->GetComponent<SaberModelContainer*>();

        _localOriginalLeftPrefab = _localLeftContainer->_saberModelControllerPrefab;
        _localOriginalRightPrefab = _localRightContainer->_saberModelControllerPrefab;

        auto registration = std::make_shared<SaberModelRegistration>(_localLeftContainer->_saberModelControllerPrefab, _localRightContainer->_saberModelControllerPrefab, -1);
        _defaultSaberModelRegistration = SaberModelRegistrationWrapper::Make(registration);

        if (!saberModelRegistrations || saberModelRegistrations.size() == 0) {
            _activeSaberModelRegistration = _defaultSaberModelRegistration;
        } else {
            std::vector<SaberModelRegistrationWrapper*> registrations;
            registrations.reserve(saberModelRegistrations.size() + 1);
            registrations.insert(registrations.begin(), saberModelRegistrations.begin(), saberModelRegistrations.end());
            registrations.push_back(_defaultSaberModelRegistration);

            std::stable_sort(registrations.begin(), registrations.end(), [](SaberModelRegistrationWrapper* a, SaberModelRegistrationWrapper* b){
                return b->_registration->operator <(*a->_registration);
            });

            _activeSaberModelRegistration = registrations[0];
        }
    }

    GlobalNamespace::SaberModelController* SaberModelProvider::NewModel(std::optional<GlobalNamespace::SaberType> saberType) {
        auto newModel = CreateNew(saberType.value_or(SaberType::SaberA));
        for (auto glow : newModel->_setSaberGlowColors) {
            if (!saberType.has_value())
                _earlyInittingGlowColors->Add(glow);
            glow->saberType = saberType.value_or(SaberType::SaberA);
        }

        for (auto fakeGlow : newModel->_setSaberFakeGlowColors) {
            if (!saberType.has_value())
                _earlyInittingFakeGlowColors->Add(fakeGlow);
            fakeGlow->saberType = saberType.value_or(SaberType::SaberA);
        }

        if (!saberType.has_value())
            SaberUtil::SetColor(newModel, {1, 1, 1, 1});
        return newModel;
    }

    GlobalNamespace::SaberModelController* SaberModelProvider::CreateNew(GlobalNamespace::SaberType saberType) {
        GlobalNamespace::SaberModelController* newModel = nullptr;
        auto reg = _activeSaberModelRegistration->_registration;
        if (reg->_leftType && reg->_rightType) {
            auto defaultPrefab = saberType == SaberType::SaberA ? _localOriginalLeftPrefab : _localOriginalRightPrefab;
            auto t = saberType == SaberType::SaberA ? reg->_leftType : reg->_rightType;
            auto go = UnityEngine::GameObject::New_ctor(t->name);
            go->SetActive(false);

            auto defaultTrail = defaultPrefab->_saberTrail;
            auto pseudoTrail = go->AddComponent<SaberTrail*>();

            pseudoTrail->_granularity = defaultTrail->_granularity;
            pseudoTrail->_trailDuration = defaultTrail->_trailDuration;
            pseudoTrail->_samplingFrequency = defaultTrail->_samplingFrequency;
            pseudoTrail->_whiteSectionMaxDuration = defaultTrail->_whiteSectionMaxDuration;
            pseudoTrail->_trailRendererPrefab = defaultTrail->_trailRendererPrefab;
            pseudoTrail->set_enabled(false);

            newModel = _container->InstantiateComponent(reinterpret_cast<System::Type*>(il2cpp_utils::GetSystemType(t)), go).cast<SaberModelController>();
            newModel->_saberTrail = pseudoTrail;
            newModel->_setSaberGlowColors = ArrayW<UnityW<SetSaberGlowColor>>(il2cpp_array_size_t(0));
            newModel->_setSaberFakeGlowColors = ArrayW<UnityW<SetSaberFakeGlowColor>>(il2cpp_array_size_t(0));
            go->SetActive(true);
        } else if (reg->_leftTemplate && reg->_rightTemplate) {
            newModel = _container->InstantiatePrefab(saberType == SaberType::SaberA ? reg->_leftTemplate.ptr() : reg->_rightTemplate.ptr())->GetComponent<SaberModelController*>();
        } else if (reg->_leftInstruction && reg->_rightInstruction) {
            newModel = saberType == SaberType::SaberA ? reg->_leftInstruction() : reg->_rightInstruction();
            _container->InjectGameObject(newModel->get_gameObject());
        } else {
            throw std::runtime_error("Invalid Saber Registration");
        }
        return newModel;
    }

    void SaberModelProvider::Dispose() {
        instance = nullptr;
    }


    bool SaberModelProvider::SetSaberGlowColor_Start_Prefix(GlobalNamespace::SetSaberGlowColor* self) {
        if (_earlyInittingGlowColors->Contains(self)) {
            _earlyInittingGlowColors->Remove(self);
            return false;
        }
        return true;
    }

    bool SaberModelProvider::SetSaberFakeGlowColor_Start_Prefix(GlobalNamespace::SetSaberFakeGlowColor* self) {
        if (_earlyInittingFakeGlowColors->Contains(self)) {
            if (self->_saberTypeObject)
                self->_saberType = self->_saberTypeObject->get_saberType();

            _earlyInittingFakeGlowColors->Remove(self);
            return false;
        }
        return true;
    }

    void SaberModelProvider::SaberModelContainer_Start_Prefix(GlobalNamespace::SaberModelContainer* self) {
        if (self != _localLeftContainer && self != _localRightContainer) return;
        if (_activeSaberModelRegistration == _defaultSaberModelRegistration) return;

        self->_saberModelControllerPrefab = NewModel(self->_saber->get_saberType());
    }
}
