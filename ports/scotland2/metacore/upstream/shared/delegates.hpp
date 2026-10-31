#pragma once

#include "System/Action.hpp"
#include "System/Action_1.hpp"
#include "System/Action_11.hpp"
#include "System/Action_2.hpp"
#include "System/Action_3.hpp"
#include "System/Action_4.hpp"
#include "System/Action_5.hpp"
#include "System/Action_6.hpp"
#include "System/Action_7.hpp"
#include "System/Action_8.hpp"
#include "UnityEngine/Events/UnityAction.hpp"
#include "UnityEngine/Events/UnityAction_1.hpp"
#include "UnityEngine/Events/UnityAction_2.hpp"
#include "UnityEngine/Events/UnityAction_3.hpp"
#include "UnityEngine/Events/UnityAction_4.hpp"
#include "custom-types/shared/delegate.hpp"

namespace MetaCore::Delegates {
    /// @brief Creates a System::Action from a std::function
    /// @tparam Targs The argument types of the function
    /// @param fun The function for the action
    /// @return System::Action_N<Targs...>*
    template <typename... Targs>
    auto MakeSystemAction(std::function<void(Targs...)> const& fun) {
        constexpr int argc = sizeof...(Targs);
        if constexpr (argc == 0)
            return custom_types::MakeDelegate<System::Action*>(fun);
        else if constexpr (argc == 1)
            return custom_types::MakeDelegate<System::Action_1<Targs...>*>(fun);
        else if constexpr (argc == 2)
            return custom_types::MakeDelegate<System::Action_2<Targs...>*>(fun);
        else if constexpr (argc == 3)
            return custom_types::MakeDelegate<System::Action_3<Targs...>*>(fun);
        else if constexpr (argc == 4)
            return custom_types::MakeDelegate<System::Action_4<Targs...>*>(fun);
        else if constexpr (argc == 5)
            return custom_types::MakeDelegate<System::Action_5<Targs...>*>(fun);
        else if constexpr (argc == 6)
            return custom_types::MakeDelegate<System::Action_6<Targs...>*>(fun);
        else if constexpr (argc == 7)
            return custom_types::MakeDelegate<System::Action_7<Targs...>*>(fun);
        else if constexpr (argc == 8)
            return custom_types::MakeDelegate<System::Action_8<Targs...>*>(fun);
        else if constexpr (argc == 11)
            return custom_types::MakeDelegate<System::Action_11<Targs...>*>(fun);

        static_assert(argc != 9, "System::Action_9 does not exist");
        static_assert(argc != 10, "System::Action_10 does not exist");
        static_assert(argc < 12, "System::Action_12 and higher do not exist");
    }

    /// @brief Creates a System::Action from a lambda
    /// @tparam T The type of the lambda
    /// @param fun The lambda for the action
    /// @return System::Action_N<Targs...>*
    template <typename T>
    auto MakeSystemAction(T fun) {
        return MakeSystemAction(std::function(fun));
    }

    /// @brief Creates a UnityEngine::Events::UnityAction from a std::function
    /// @tparam Targs The argument types of the function
    /// @param fun The function for the action
    /// @return UnityEngine::Events::UnityAction_N<Targs>*
    template <typename... Targs>
    auto MakeUnityAction(std::function<void(Targs...)> fun) {
        constexpr int argc = sizeof...(Targs);
        if constexpr (argc == 0)
            return custom_types::MakeDelegate<UnityEngine::Events::UnityAction*>(fun);
        else if constexpr (argc == 1)
            return custom_types::MakeDelegate<UnityEngine::Events::UnityAction_1<Targs...>*>(fun);
        else if constexpr (argc == 2)
            return custom_types::MakeDelegate<UnityEngine::Events::UnityAction_2<Targs...>*>(fun);
        else if constexpr (argc == 3)
            return custom_types::MakeDelegate<UnityEngine::Events::UnityAction_3<Targs...>*>(fun);
        else if constexpr (argc == 4)
            return custom_types::MakeDelegate<UnityEngine::Events::UnityAction_4<Targs...>*>(fun);

        static_assert(argc < 5, "UnityEngine::Events::UnityAction_5 and higher do not exist");
    }

    /// @brief Creates a UnityEngine::Events::UnityAction from a lambda
    /// @tparam T The type of the lambda
    /// @param fun The function for the action
    /// @return UnityEngine::Events::UnityAction_N<Targs>*
    template <typename T>
    auto MakeUnityAction(T fun) {
        return MakeUnityAction(std::function(fun));
    }

    inline bool SafeIsNull(System::MulticastDelegate* delegate) {
        return System::MulticastDelegate::op_Equality(delegate, nullptr);
    }
}
