#pragma once

#include "custom-types/shared/macros.hpp"
#include "UnityEngine/Vector2.hpp"
#include "UnityEngine/RectTransform.hpp"
#include "TMPro/TextMeshProUGUI.hpp"
#include "UnityEngine/UI/Button.hpp"
#include "Key.hpp"
#include <functional>

DECLARE_CLASS_CODEGEN(BSML, Keyboard, System::Object) {
    DECLARE_INSTANCE_FIELD(ListW<Key*>, keys);
    DECLARE_INSTANCE_FIELD(Key*, dummy);

    DECLARE_INSTANCE_FIELD(bool, enableInputField);     /* default: true;  */
    DECLARE_INSTANCE_FIELD(bool, shift);                /* default: false; */
    DECLARE_INSTANCE_FIELD(bool, caps);                 /* default: false; */
    DECLARE_INSTANCE_FIELD(UnityEngine::RectTransform*, container);
    DECLARE_INSTANCE_FIELD(UnityEngine::Vector2, currentPosition);
    DECLARE_INSTANCE_FIELD(UnityEngine::Vector2, basePosition);
    DECLARE_INSTANCE_FIELD(float, scale);               /* default: 0.5f;  */
    DECLARE_INSTANCE_FIELD(float, padding);             /* default: 0.5f;  */
    DECLARE_INSTANCE_FIELD(float, buttonWidth);         /* default: 12.0f; */

    DECLARE_INSTANCE_FIELD(TMPro::TextMeshProUGUI*, keyboardText);
    DECLARE_INSTANCE_FIELD(TMPro::TextMeshProUGUI*, keyboardCursor);
    DECLARE_INSTANCE_FIELD(UnityEngine::UI::Button*, baseButton);

    DECLARE_CTOR(ctor);

    public:
        static const std::string QWERTY;
        static const std::string FKEYROW;
        static const std::string NUMPAD;
        static const std::string DVORAK;

        void add_text(StringW value);

        Key* get_key(StringW index);
        void SetButtonType(std::string_view ButtonName);
        void SetValue(std::string_view keyLabel, std::string_view value);
        void SetAction(std::string_view keyName, std::function<void(Key*)> action);
        Keyboard* AddKeys(std::string_view keyboard, float scale = 0.5f);
        Keyboard* DefaultActions();
        Keyboard* NextRow(float adjustX = 0);
        Keyboard* SetScale(float scale);
        void Clear(Key* key);
        void Enter(Key* key);
        void Backspace(Key* key);
        void Shift(Key* key);
        void Caps(Key* key);
        void DrawCursor();

        std::function<void(StringW)> enterPressed = nullptr;

        static Keyboard* construct(UnityEngine::RectTransform* container, std::string_view defaultKeyboard = QWERTY, bool enableInputField = false, float x = 0, float y = 0);
    private:
        Key* AddKey(StringW keyLabel, float width = 12.0f, float height = 10.0f, int color = 0xffffff);
        Key* AddKey(StringW keyLabel, StringW shifted, float width = 12.0f, float height = 10.0f);
        void EmitKey(float& spacing, float& width, StringW& label, StringW& key, bool& space, StringW& newValue, float& height, int& color);
        bool ReadFloat(StringW data, int& position, float& result);
};