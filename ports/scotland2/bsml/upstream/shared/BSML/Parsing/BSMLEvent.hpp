#pragma once

#include "../../_config.h"
#include <functional>
#include <vector>

namespace BSML {
    class BSML_EXPORT BSMLEvent {
        public:
            BSMLEvent() {};
            BSMLEvent(const std::function<void(void)>& func) { Add(func); };

            void Invoke() {
                for (auto& f : funcs) {
                    f();
                }
            }

            void Add(std::function<void()> func) {
                if (func == nullptr) return;
                funcs.push_back(func);
            }
        private:
            std::vector<std::function<void(void)>> funcs;
    };
}
