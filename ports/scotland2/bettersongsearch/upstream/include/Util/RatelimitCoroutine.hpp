#pragma once

#include "custom-types/shared/coroutine.hpp"
#include "custom-types/shared/macros.hpp"
#include "UnityEngine/WaitForSeconds.hpp"

namespace BetterSongSearch::Util {
    class RatelimitCoroutine {
        std::function<void()> exitfn;
        float limit;

       public:
        RatelimitCoroutine(std::function<void()> exitfn, float limit = 0.5f) {
            this->exitfn = exitfn;
            this->limit = limit;
        }

        bool wasRecentlyExecuted = false;
        bool queuedFallingEdge = false;

        custom_types::Helpers::Coroutine Call() {
            if (!wasRecentlyExecuted) {
                wasRecentlyExecuted = true;
                co_yield custom_types::Helpers::new_coro(CallNow());
            } else {
                queuedFallingEdge = true;
            }
            co_return;
        }

        custom_types::Helpers::Coroutine CallNextFrame() {
            co_yield nullptr;
            co_yield custom_types::Helpers::new_coro(Call());
            co_return;
        }

        custom_types::Helpers::Coroutine CallNow() {
            exitfn();

            co_yield reinterpret_cast<System::Collections::IEnumerator*>(UnityEngine::WaitForSeconds::New_ctor(limit));

            if (queuedFallingEdge) {
                queuedFallingEdge = false;
                co_yield custom_types::Helpers::new_coro(CallNow());
            } else {
                wasRecentlyExecuted = false;
            }
            co_return;
        }
    };
};  // namespace BetterSongSearch::Util
