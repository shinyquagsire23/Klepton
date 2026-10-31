#include "BSML/Tags/ModalKeyboardTag.hpp"
#include "BSML/Components/ModalView.hpp"
#include "BSML/Components/ExternalComponents.hpp"
#include "logging.hpp"

#include "UnityEngine/GameObject.hpp"
#include "UnityEngine/Resources.hpp"
#include "UnityEngine/Vector3.hpp"
#include "UnityEngine/UI/Image.hpp"
#include "UnityEngine/UI/ScrollRect.hpp"
#include "HMUI/TableView.hpp"
#include "HMUI/ScrollView.hpp"
#include "HMUI/EventSystemListener.hpp"
#include "VRUIControls/VRGraphicRaycaster.hpp"

#include "BSML/Components/Keyboard/ModalKeyboard.hpp"

using namespace HMUI;
using namespace UnityEngine;
using namespace UnityEngine::UI;
using namespace VRUIControls;

static inline UnityEngine::Vector3 operator*(UnityEngine::Vector3 vec, float v) {
    return { vec.x * v, vec.y * v, vec.z * v };
}

namespace BSML {
    static BSMLNodeParser<ModalKeyboardTag> modalKeyboardTagParser({"modal-keyboard"});
    UnityEngine::GameObject* ModalKeyboardTag::CreateObject(UnityEngine::Transform* parent) const {
        DEBUG("Creating Modal Keyboard");
        auto gameObject = ModalTag::CreateObject(parent);
        auto transform = gameObject->transform.cast<RectTransform>();
        gameObject->set_name("BSMLModalKeyboard");
        transform->set_sizeDelta({135, 75});

        auto parentTransform = GameObject::New_ctor("KeyboardParent")->AddComponent<RectTransform*>();
        parentTransform->SetParent(transform, false);
        parentTransform->set_localScale(parentTransform->get_localScale() * 1.4f);
        auto keyboard = Keyboard::construct(parentTransform, Keyboard::QWERTY, true, 4, -12);

        auto modalKeyboard = gameObject->AddComponent<ModalKeyboard*>();
        modalKeyboard->keyboard = keyboard;
        modalKeyboard->modalView = gameObject->GetComponent<BSML::ModalView*>();
        keyboard->enterPressed = std::bind(&ModalKeyboard::OnEnter, modalKeyboard, std::placeholders::_1);

        auto externalComponents = gameObject->GetComponent<ExternalComponents*>();
        externalComponents->Add(modalKeyboard);

        return gameObject;
    }
}
