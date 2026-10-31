#pragma once

#include "../../_config.h"
#include "_config.h"
#include "custom-types/shared/macros.hpp"
#include "TMPro/TMP_Text.hpp"
#include "UnityEngine/MonoBehaviour.hpp"

namespace BSML {
    /// @brief Gradient class, if you want custom Gradient behaviour then implement your own class and set it on the GradientUpdater using the set_gradient method
    class BSML_EXPORT Gradient {
        public:
            /// @brief Sample the gradient represented by this instance
            /// @param t where to sample
            /// @return UnityEngine Color32
            virtual UnityEngine::Color32 Sample(float t) const = 0;
            static Gradient* Parse(std::string_view str);
            virtual ~Gradient() {};
    };

    /// @brief This type really only exists to speed up the math if you just input 2 colors, mostly for non-animated gradients
    class BSML_EXPORT SimpleTwoColorGradient : public Gradient {
        public:
            /// @brief Lerps between the start and end colors
            /// @param t lerp distance
            /// @return UnityEngine Color32
            UnityEngine::Color32 Sample(float t) const override;
            UnityEngine::Color32 start;
            UnityEngine::Color32 end;
    };

    /// @brief Color gradient that samples from the passed vector
    class BSML_EXPORT SimpleColorGradient : public Gradient {
        public:
            /// @brief Lerps between the colors in the vector
            /// @param t lerp distance
            /// @return UnityEngine Color32
            UnityEngine::Color32 Sample(float t) const override;
            std::vector<UnityEngine::Color32> colors;
    };
}

DECLARE_CLASS_CODEGEN(BSML, TextGradientUpdater, UnityEngine::MonoBehaviour) {
    DECLARE_INSTANCE_FIELD(TMPro::TMP_Text*, text);
    DECLARE_INSTANCE_FIELD(float, scrollSpeed);
    DECLARE_INSTANCE_FIELD(float, scrollRepeat);
    DECLARE_INSTANCE_FIELD(float, stepSize);
    DECLARE_INSTANCE_FIELD(bool, fixedStep);
    DECLARE_INSTANCE_FIELD(float, currentPos);

    DECLARE_INSTANCE_METHOD(void, Awake);
    DECLARE_INSTANCE_METHOD(void, OnDestroy);
    DECLARE_INSTANCE_METHOD(void, LateUpdate);
    DECLARE_CTOR(ctor);
    public:
        Gradient* get_gradient();
        void set_gradient(Gradient* gradient);
    private:
        Gradient* gradient;
};