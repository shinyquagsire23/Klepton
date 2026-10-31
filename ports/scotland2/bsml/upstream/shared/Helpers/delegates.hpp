#pragma once

/// A header with templated methods to simplify making delegates

#include "custom-types/shared/delegate.hpp"
#include "System/Action.hpp"
#include "System/Action_1.hpp"
#include "System/Action_2.hpp"
#include "System/Action_3.hpp"
#include "System/Action_4.hpp"
#include "System/Action_5.hpp"
#include "System/Action_6.hpp"
#include "System/Action_7.hpp"
#include "System/Action_8.hpp"
#include "System/Action_11.hpp"

#include "UnityEngine/Events/UnityAction.hpp"
#include "UnityEngine/Events/UnityAction_1.hpp"
#include "UnityEngine/Events/UnityAction_2.hpp"
#include "UnityEngine/Events/UnityAction_3.hpp"
#include "UnityEngine/Events/UnityAction_4.hpp"

#include <sstream>

namespace BSML {
    /// @brief struct to check argcount
    template <int argc, int check>
    struct check_count_equals {
        static_assert(argc == check, "argument count was not equal");
    };

    /// @brief struct to check argcount
    template <int argc, int check>
    struct check_count_not_equals {
        static_assert(argc != check, "argument count was equal");
    };

    /// @brief struct to check argcount
    template <int argc, int check>
    struct check_count_smaller {
        static_assert(argc <= check, "argument count was greater than supported");
    };

    /// @brief a templated method to put the MakeDelegate call into a function so that when we change where the MakeDelegate call is, we just change this method
    /// @tparam T type of the delegate
    /// @tparam F whatever you want to pass into MakeDelegate
    /// @param fun whatever you want to pass into MakeDelegate
    /// @return T delegate
    template <typename T, typename F>
    inline T MakeDelegate(F fun) {
        return custom_types::MakeDelegate<T>(fun);
    }

    /// @brief templated method to turn an std::function into an action
    /// @tparam Targs the types of the arguments of the function
    /// @param fun the function
    /// @return System::Action_N<Targs>
    template<typename... Targs>
    auto MakeSystemAction(const std::function<void(Targs...)>& fun) {
        constexpr int argc = sizeof...(Targs);
        if constexpr (argc == 0) return MakeDelegate<System::Action*>(fun);
        else if constexpr (argc == 1) return MakeDelegate<System::Action_1<Targs...>*>(fun);
        else if constexpr (argc == 2) return MakeDelegate<System::Action_2<Targs...>*>(fun);
        else if constexpr (argc == 3) return MakeDelegate<System::Action_3<Targs...>*>(fun);
        else if constexpr (argc == 4) return MakeDelegate<System::Action_4<Targs...>*>(fun);
        else if constexpr (argc == 5) return MakeDelegate<System::Action_5<Targs...>*>(fun);
        else if constexpr (argc == 6) return MakeDelegate<System::Action_6<Targs...>*>(fun);
        else if constexpr (argc == 7) return MakeDelegate<System::Action_7<Targs...>*>(fun);
        else if constexpr (argc == 8) return MakeDelegate<System::Action_8<Targs...>*>(fun);
        else if constexpr (argc == 11) return MakeDelegate<System::Action_11<Targs...>*>(fun);

        /// 9 is not allowed, System::Action_9 does not exist
        static check_count_not_equals<argc, 9> check_9;
        /// 10 is not allowed, System::Action_10 does not exist
        static check_count_not_equals<argc, 10> check_10;
        /// > 11 is not allowed, System::Action_N does not exist with N > 11
        static check_count_smaller<argc, 11> check_11;
    }

    /// @brief templated method to turn lambdas into actions
    /// @tparam T the type of the lambda
    /// @tparam Targs the types of the arguments of the lambda
    /// @param fun the lambda
    /// @return System::Action_N<Targs>
    template<typename T, typename...Targs>
    requires(std::is_constructible_v<std::function<void(Targs...)>, T> && !std::is_same_v<std::function<void(Targs...)>, T>)
    auto MakeSystemAction(T fun) {
        return MakeSystemAction<Targs...>(std::function<void(Targs...)>(fun));
    }

    /// @brief templated method to turn an instance + methodInfo into an action
    /// @tparam Targs the types of the arguments of the lambda
    /// @param instance the object instance
    /// @param methodInfo the methodInfo of the method to run on instance
    /// @return System::Action_N<Targs>
    template<typename...Targs>
    auto MakeSystemAction(System::Object* instance, const MethodInfo* methodInfo) {
        if (methodInfo->parameters_count != sizeof...(Targs)) {
            std::stringstream strm;
            strm << "Argcount mismatch between methodInfo and Targs: " << methodInfo->parameters_count << "!=" << sizeof...(Targs);
            throw std::runtime_error(strm.str());
        }
        return MakeSystemAction<Targs...>(std::function<void(Targs...)>([instance, methodInfo](Targs... args){
            il2cpp_utils::RunMethod(instance, methodInfo, args...);
        }));
    }

    /// @brief templated method to turn an std::function into an action
    /// @tparam Targs the types of the arguments of the function
    /// @param fun the function
    /// @return UnityEngine::Events::UnityAction_N<Targs>
    template<typename... Targs>
    auto MakeUnityAction(std::function<void(Targs...)> fun) {
        constexpr int argc = sizeof...(Targs);
        if constexpr (argc == 0) return MakeDelegate<UnityEngine::Events::UnityAction*>(fun);
        else if constexpr (argc == 1) return MakeDelegate<UnityEngine::Events::UnityAction_1<Targs...>*>(fun);
        else if constexpr (argc == 2) return MakeDelegate<UnityEngine::Events::UnityAction_2<Targs...>*>(fun);
        else if constexpr (argc == 3) return MakeDelegate<UnityEngine::Events::UnityAction_3<Targs...>*>(fun);
        else if constexpr (argc == 4) return MakeDelegate<UnityEngine::Events::UnityAction_4<Targs...>*>(fun);

        /// > 4 is not allowed, UnityEngine::Events::UnityAction_N does not exist with N > 4
        static check_count_smaller<argc, 4> check_4;
    }

    /// @brief templated method to turn lambdas into actions
    /// @tparam T the type of the lambda
    /// @tparam Targs the types of the arguments of the lambda
    /// @param fun the lambda
    /// @return UnityEngine::Events::UnityAction_N<Targs>
    template<typename T, typename...Targs>
    requires(std::is_constructible_v<std::function<void(Targs...)>, T> && !std::is_same_v<std::function<void(Targs...)>, T>)
    auto MakeUnityAction(T fun) {
        return MakeUnityAction<Targs...>(std::function<void(Targs...)>(fun));
    }

    /// @brief templated method to turn an instance + methodInfo into an action
    /// @tparam Targs the types of the arguments of the lambda
    /// @param instance the object instance
    /// @param methodInfo the methodInfo of the method to run on instance
    /// @return UnityEngine::Events::UnityAction_N<Targs>
    template<typename...Targs>
    auto MakeUnityAction(System::Object* instance, const MethodInfo* methodInfo) {
        if (methodInfo->parameters_count != sizeof...(Targs)) {
            std::stringstream strm;
            strm << "Argcount mismatch between methodInfo and Targs: " << methodInfo->parameters_count << "!=" << sizeof...(Targs);
            throw std::runtime_error(strm.str());
        }
        return MakeUnityAction<Targs...>(std::function<void(Targs...)>([instance, methodInfo](Targs... args){
            il2cpp_utils::RunMethod(instance, methodInfo, args...);
        }));
    }
}
