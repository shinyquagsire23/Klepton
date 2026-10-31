#include "BSML/Components/Keyboard/Keyboard.hpp"
#include "BSML/Components/Keyboard/Key.hpp"
#include "Helpers/creation.hpp"
#include "Helpers/extension.hpp"

#include "System/Single.hpp"
#include "System/Int32.hpp"

#include "UnityEngine/UI/Image.hpp"
#include "UnityEngine/UI/Button.hpp"
#include "UnityEngine/Resources.hpp"
#include "System/Globalization/NumberStyles.hpp"

DEFINE_TYPE(BSML, Keyboard);

using namespace UnityEngine;
using namespace UnityEngine::UI;

namespace BSML {
        const std::string Keyboard::QWERTY =
"[CLEAR]/20\n\
(`~) (1!) (2@) (3#) (4$) (5%) (6^) (7&) (8*) (9() (0)) (-_) (=+) [<--]/15\n\
[TAB]/15'\t' (qQ) (wW) (eE) (rR) (tT) (yY) (uU) (iI) (oO) (pP) ([{) (]}) (\\|)\n\
[CAPS]/20 (aA) (sS) (dD) (fF) (gG) (hH) (jJ) (kK) (lL) (;:) ('"") [ENTER]/20,22#20A0D0\n\
[SHIFT]/25 (zZ) (xX) (cC) (vV) (bB) (nN) (mM) (,<) (.>) (/?)  \n\
/23 (!!) (@@) [SPACE]/40' ' (##) (__)";

        const std::string Keyboard::FKEYROW =
"\n\
[Esc] /2 [F1] [F2] [F3] [F4] /2 [F5] [F6] [F7] [F8] /2 [F9] [F10] [F11] [F12]\n\
";
        const std::string Keyboard::NUMPAD =
"\n\
[NUM] (//) (**) (--)\n\
(77) (88) (99) [+]/10,22\n\
(44) (55) (66)\n\
(11) (22) (33) [ENTER]/10,22\n\
[0]/22 [.]\n\
";
        const std::string Keyboard::DVORAK =
"\n\
(`~) (1!) (2@) (3#) (4$) (5%) (6^) (7&) (8*) (9() (0)) ([{) (]}) [<--]/15\n\
[TAB]/15 ('"") (,<) (.>) (pP) (yY) (fF) (gG) (cC) (rR) (lL) (/?) (=+) (\\|)\n\
[CAPS]/20 (aA) (oO) (eE) (uU) (iI) (dD) (hH) (tT) (nN) (sS) (-_) [ENTER]/20\n\
[SHIFT] (;:) (qQ) (jJ) (kK) (xX) (bB) (mM) (wW) (vV) (zZ) [CLEAR]/28\n\
/23 (!!) (@@) [SPACE]/40 (##) (__)";

        void Keyboard::ctor() {
            keys = ListW<Key*>::New();
            dummy = Key::New_ctor();
            enableInputField = true;
            shift = false;
            caps = false;
            scale = 0.5f;
            padding = 0.5f;
            buttonWidth = 12.0f;
        }


        void Keyboard::add_text(StringW value) {
            if (keyboardText)
                keyboardText->set_text(keyboardText->get_text() + value);
        }

        Key* Keyboard::get_key(StringW index) {
            for(auto& key : keys) {
                if (StringW(key->name) == index) return key;
            }

            return dummy;
        }

        void Keyboard::SetButtonType(std::string_view buttonName) {
            auto allButtons = Resources::FindObjectsOfTypeAll<Button*>();
            Button* q;
            baseButton = allButtons->First([&](auto x){
                if (x->get_name() == "Q") q = x;
                return x->get_name() == buttonName;
            });

            if (!baseButton) {
                baseButton = q;
            }
        }

        void Keyboard::SetValue(std::string_view keyLabel, std::string_view value) {
            auto key = get_key(keyLabel);
            if (key != dummy) {
                key->value = value;
            }
        }

        void Keyboard::SetAction(std::string_view keyLabel, std::function<void(Key*)> action) {
            auto key = get_key(keyLabel);
            if (key != dummy) {
                key->keyAction = action;
            }
        }

        Keyboard* Keyboard::AddKeys(std::string_view keyboard_view, float scale) {
            StringW keyboard{keyboard_view};
            int keyboardLength = keyboard->get_Length();
            this->scale = scale;
            bool space = true;
            float spacing = padding;
            float width = buttonWidth;
            float height = 10.0f;
            StringW label = "";
            StringW key = "";
            StringW newValue = "";
            int color = 0xffffff;
            int p = 0; // P is for parser

            while (p < keyboardLength) {
                switch (keyboard[p]) {
                    case '@': // position key
                        EmitKey(spacing, width, label, key, space, newValue, height, color);
                        p++;
                        if (ReadFloat(keyboard, p, currentPosition.x)) {
                            basePosition.x = currentPosition.x;
                            if (p < keyboardLength && keyboard[p] == ',') {
                                p++;
                                ReadFloat(keyboard, p, currentPosition.y);
                                basePosition.y = currentPosition.y;
                            }
                        }
                        continue;
                    case 'S': // scale
                        EmitKey(spacing, width, label, key, space, newValue, height, color);
                        p++;
                        ReadFloat(keyboard, p, this->scale);
                        continue;
                    case '\r':
                        space = true;
                        break;
                    case '\n':
                        EmitKey(spacing, width, label, key, space, newValue, height, color);
                        space = true;
                        NextRow();
                        break;
                    case ' ':
                        space = true;
                        break;
                    case '[': {
                        EmitKey(spacing, width, label, key, space, newValue, height, color);
                        space = false;
                        p++;
                        int labelStart = p;
                        // goto end of label
                        while (p < keyboardLength && keyboard[p] != ']') p++;
                        label = keyboard->Substring(labelStart, p - labelStart);
                        break;
                    }
                    case '(':
                        EmitKey(spacing, width, label, key, space, newValue, height, color);
                        p++;
                        key = keyboard->Substring(p, 2);
                        p += 2;
                        space = false;
                        break;
                    case '#':
                        // IMPORTANT -> this does not support anything that's not rgb hex (#ffffff)
                        p++;
                        color = System::Int32::Parse(keyboard->Substring(p, 6), System::Globalization::NumberStyles::HexNumber);
                        p   += 6;
                        continue;
                    case '/': {
                        p++;
                        float number = 0;
                        if (ReadFloat(keyboard, p, number)) {
                            if (p < keyboardLength && keyboard[p] == ',') {
                                p++;
                                ReadFloat(keyboard, p, height);
                            }

                            if (space) {
                                if (!System::String::IsNullOrEmpty(label) || !System::String::IsNullOrEmpty(key))
                                    EmitKey(spacing, width, label, key, space, newValue, height, color);
                                spacing = number;
                            } else {
                                width = number;
                            }

                            continue;
                        }
                        break;
                    }
                        // this one looks like its \, but it's actually just '
                    case '\'': {
                        p++;
                        int newValueP = p;
                        while (p < keyboardLength && keyboard[p] != '\'') p++;
                        newValue = keyboard->Substring(newValueP, p - newValueP);
                        break;
                    }
                    default:
                        return this;
                }
                p++;
            }

            EmitKey(spacing, width, label, key, space, newValue, height, color);

            return this;
        }

        Keyboard* Keyboard::DefaultActions() {
            SetAction("CLEAR", std::bind(&Keyboard::Clear, this, std::placeholders::_1));
            SetAction("ENTER", std::bind(&Keyboard::Enter, this, std::placeholders::_1));
            SetAction("<--", std::bind(&Keyboard::Backspace, this, std::placeholders::_1));
            SetAction("SHIFT", std::bind(&Keyboard::Shift, this, std::placeholders::_1));
            SetAction("CAPS", std::bind(&Keyboard::Caps, this, std::placeholders::_1));

            return this;
        }

        Keyboard* Keyboard::NextRow(float adjustX) {
            currentPosition.y -= currentPosition.x == basePosition.x ? 3 : 6;
            currentPosition.x = basePosition.x;
            return this;
        }

        Keyboard* Keyboard::SetScale(float scale) {
            this->scale = scale;
            return this;
        }

        void Keyboard::Clear(Key* key) {
            key->kb->keyboardText->set_text("");
        }

        void Keyboard::Enter(Key* key) {
            StringW typedText = key->kb->keyboardText->get_text();
            if (enterPressed)
                enterPressed(typedText ? typedText : "");
            key->kb->keyboardText->set_text("");
        }

        void Keyboard::Backspace(Key* key) {
            auto text = key->kb->keyboardText->get_text();
            int length = text ? text->get_Length() : 0;
            if (length > 0) {
                key->kb->keyboardText->set_text(text->Remove(length -1));
            }
        }

        void Keyboard::Shift(Key* key) {
            key->kb->shift = !key->kb->shift;

            bool isShift = key->kb->shift;
            auto& keys = key->kb->keys;
            for(auto k : keys) {
                auto x = isShift ? k->shifted : k->value;
                if (!System::String::IsNullOrWhiteSpace(k->shifted)) {
                    Helpers::SetButtonText(k->button, x);
                }
            }
            key->button->GetComponentInChildren<Image*>()->set_color(isShift ? Color{0.0f, 1.0f, 0.0f, 1.0f} : Color{1.0f, 1.0f, 1.0f, 1.0f});
        }

        void Keyboard::Caps(Key* key) {
            key->kb->caps = !key->kb->caps;
            key->button->GetComponentInChildren<Image*>()->set_color(key->kb->caps ? Color{0.0f, 1.0f, 0.0f, 1.0f} : Color{1.0f, 1.0f, 1.0f, 1.0f});
        }

        void Keyboard::DrawCursor() {
            if (!enableInputField) return;

            auto v = keyboardText->GetPreferredValues(keyboardText->get_text() + "I");
            v.y = 15.0f;
            v.x = v.x / 2 + 30.0f - 0.5f;
            keyboardCursor->transform.cast<RectTransform>()->set_anchoredPosition(v);
        }

        Keyboard* Keyboard::construct(UnityEngine::RectTransform* container, std::string_view defaultKeyboard, bool enableInputField, float x, float y) {
            auto self = Keyboard::New_ctor();
            self->enableInputField = enableInputField;
            self->container = container;
            self->basePosition = {-50 + x, 23 + y};
            self->currentPosition = self->basePosition;

            self->SetButtonType("Q");

            auto keyboardText = Helpers::CreateText(container, "", {0, 15.0f});
            self->keyboardText = keyboardText;
            keyboardText->set_fontSize(6.0f);
            keyboardText->set_color({1.0f, 1.0f, 1.0f, 1.0f});
            keyboardText->set_alignment(TMPro::TextAlignmentOptions::Center);
            keyboardText->set_enableWordWrapping(false);
            keyboardText->set_text("");
            keyboardText->set_enabled(self->enableInputField);

            auto keyboardCursor = Helpers::CreateText(container, "I", {0, 0});
            self->keyboardCursor = keyboardCursor;
            keyboardCursor->set_fontSize(6.0f);
            keyboardCursor->set_color({0.60f, 0.80f, 1.0f, 1.0f});
            keyboardCursor->set_alignment(TMPro::TextAlignmentOptions::Left);
            keyboardCursor->set_enableWordWrapping(false);
            keyboardCursor->set_enabled(self->enableInputField);

            self->DrawCursor();

            if (defaultKeyboard != "") {
                self->AddKeys(defaultKeyboard);
                self->DefaultActions();
            }

            return self;
        }

        Key* Keyboard::AddKey(StringW keyLabel, float width, float height, int color) {
            Vector2 position = currentPosition;

            Color co;
            co.r = (float)(color & 0xff) / 255.0f;
            co.g = (float)((color << 8) & 0xff) / 255.0f;
            co.b = (float)((color << 16) & 0xff) / 255.0f;
            co.a = 1.0f;

            auto key = Key::construct(this, position, keyLabel, width, height, co);
            keys->Add(key);

            return key;
        }

        Key* Keyboard::AddKey(StringW keyLabel, StringW shifted, float width, float height) {
            Key* key = AddKey(keyLabel, width, height);
            key->shifted = shifted;

            return key;
        }

        void Keyboard::EmitKey(float& spacing, float& width, StringW& label, StringW& key, bool& space, StringW& newValue, float& height, int& color) {
            currentPosition.x += spacing;

            if (!System::String::IsNullOrEmpty(label))
                AddKey(label, width, height, color)->Set(newValue);
            else if (!System::String::IsNullOrEmpty(key)) {
                auto first = std::u16string_view{&key[0], 1};
                auto second = std::u16string_view{&key[1], 1};

                AddKey(first, second)->Set(newValue);
            }

            spacing = 0;
            width = buttonWidth;
            height = 10.0f;
            label = "";
            key = "";
            newValue = "";
            color = 0xffffff;
            space = false;

            return;
        }

        bool Keyboard::ReadFloat(StringW data, int& position, float& result) {
            if (position >= data->get_Length())
                return false;

            int start = position;
            while (position < data->get_Length())
            {
                char c = data[position];
                if (!((c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.'))
                    break;

                position++;
            }

            if (System::Single::TryParse(data->Substring(start, position - start), byref(result)))
                return true;

            position = start;
            return false;
        }
}
