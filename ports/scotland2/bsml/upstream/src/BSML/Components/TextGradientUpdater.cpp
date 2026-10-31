#include "BSML/Components/TextGradientUpdater.hpp"
#include "logging.hpp"
#include "StringParseHelper.hpp"

#include "TMPro/TMP_TextInfo.hpp"
#include "TMPro/TMP_VertexDataUpdateFlags.hpp"
#include "TMPro/TMP_MeshInfo.hpp"
#include "UnityEngine/Mesh.hpp"
#include "UnityEngine/Time.hpp"

DEFINE_TYPE(BSML, TextGradientUpdater);

UnityEngine::Color32 operator+(UnityEngine::Color32 lhs, UnityEngine::Color32 rhs) {
    return {
        0,
        static_cast<uint8_t>(std::clamp(lhs.r + rhs.r, 0, 255)),
        static_cast<uint8_t>(std::clamp(lhs.g + rhs.g, 0, 255)),
        static_cast<uint8_t>(std::clamp(lhs.b + rhs.b, 0, 255)),
        static_cast<uint8_t>(std::clamp(lhs.a + rhs.a, 0, 255))
    };
}
UnityEngine::Color32 operator*(UnityEngine::Color32 lhs, float rhs) {
    return {
        0,
        static_cast<uint8_t>(std::clamp((int)(lhs.r * rhs), 0, 255)),
        static_cast<uint8_t>(std::clamp((int)(lhs.g * rhs), 0, 255)),
        static_cast<uint8_t>(std::clamp((int)(lhs.b * rhs), 0, 255)),
        static_cast<uint8_t>(std::clamp((int)(lhs.a * rhs), 0, 255))
    };
}

inline UnityEngine::Color32 operator*(float lhs, UnityEngine::Color32 rhs) {
    return rhs * lhs;
}

namespace BSML {
    void TextGradientUpdater::ctor() {
        scrollSpeed = 0;
        scrollRepeat = 1;
        stepSize = 1;
        fixedStep = false;
        gradient = nullptr;
    }

    void TextGradientUpdater::set_gradient(Gradient* gradient) {
        if (this->gradient) delete this->gradient;
        this->gradient = gradient;
    }

    Gradient* TextGradientUpdater::get_gradient() { return gradient; }

    void TextGradientUpdater::Awake() {
        text = GetComponent<TMPro::TMP_Text*>();
    }

    void TextGradientUpdater::OnDestroy() {
        delete gradient;
        gradient = nullptr;
    }

    void TextGradientUpdater::LateUpdate() {
        if (text && text->m_CachedPtr.m_value && gradient) {
            // TODO: check if it's possible to implement something where the colors only get set once if scrollSpeed == 0
            auto textInfo = text->get_textInfo();
            auto characterCount = textInfo->characterCount;
            if (characterCount > 0) {
                currentPos = std::fmod(currentPos + UnityEngine::Time::get_deltaTime() * scrollSpeed, 1.0f);
                int materialCount = textInfo->materialCount;
                if (scrollRepeat == 0) {
                    auto col = gradient->Sample(currentPos);
                    for (int i = 0; i < materialCount; i++) {
                        for (auto& c : textInfo->meshInfo[i].colors32) c = col;
                    }
                } else {
                    for (int i = 0; i < materialCount; i++) {
                        auto colors = textInfo->meshInfo[i].colors32;
                        auto size = colors.size();
                        if (fixedStep) {
                            for (int j = 0; j < size; j++) {
                                float t = j * stepSize * 0.01f;
                                colors[j] = gradient->Sample(currentPos + t);
                            }
                        } else {
                            for (int j = 0; j < size; j++) {
                                float t = (float)j / (float)size;
                                colors[j] = gradient->Sample(currentPos + t * scrollRepeat);
                            }
                        }
                    }
                }

                text->UpdateVertexData(TMPro::TMP_VertexDataUpdateFlags::Colors32);
            }
        }
    }

    UnityEngine::Color32 SimpleTwoColorGradient::Sample(float t) const {
        return t * end + (1 - t) * start;
    }

    UnityEngine::Color32 SimpleColorGradient::Sample(float t) const {
        if (colors.empty()) return {0, 255, 255, 255, 255};
        if (colors.size() == 1) return colors.at(0);
        t = std::fmod(t, 1.0f);

        float i_unfloored = t * (colors.size() - 1);
        int i = std::floor(i_unfloored);

        if (i < colors.size() - 1) {
            float subT = std::fmod(i_unfloored, 1.0f);
            return (1 - subT) * colors[i] + subT * colors[i + 1];
        } else {
            return colors[i];
        }
    }

    Gradient* Gradient::Parse(std::string_view str) {
        std::vector<StringParseHelper> gradientStrings{};
        char separator = ';';
        std::size_t start = 0, end = 0;
        while ((end = str.find(separator, start)) != std::string::npos) {
            gradientStrings.push_back(StringParseHelper(str.substr(start, end - start)));
            start = end + 1;
        }
        gradientStrings.push_back(str.substr(start));

        if (gradientStrings.size() == 2) {
            auto gradient = new SimpleTwoColorGradient();
            gradient->start = gradientStrings.at(0);
            gradient->end = gradientStrings.at(1);
            return gradient;
        } else {
            auto gradient = new SimpleColorGradient();
            for (auto c : gradientStrings) {
                gradient->colors.emplace_back(c);
            }
            return gradient;
        }
    }
}
