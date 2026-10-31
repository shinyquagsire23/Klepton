#pragma once

#include "./_config.h"
#include "UnityEngine/Transform.hpp"
#include "HMUI/ViewController.hpp"
#include "HMUI/FlowCoordinator.hpp"
#include "BSML/Parsing/BSMLParser.hpp"
#include "BSML/MenuButtons/MenuButton.hpp"
#include "BSML/GameplaySetup/MenuType.hpp"
#include "BSML/MenuSource.hpp"

// include direct creation methods
#include "BSML-Lite.hpp"

#include <memory>
#include <string>
#include <functional>

namespace BSML {
    /// @brief Intialize BSML for its hooks
    BSML_EXPORT void Init();

    /// @brief Intialize BSML late init stuff
    BSML_EXPORT void LateInit();

    /// @brief parse a string containing a BSML doc
    /// @return BSMLNode pointer to parsed hierarchy
    BSML_EXPORT std::shared_ptr<BSMLParser> parse(std::string_view str);

    /// @brief parse a string containing a BSML doc
    /// @param str the string to parse
    /// @param parent what to parent to
    /// @param host the host object, this would contain the various fields and properties your bsml expects to be able to access
    /// @return parserparams result
    BSML_EXPORT std::shared_ptr<BSMLParser> parse_and_construct(std::string_view str, UnityEngine::Transform* parent, System::Object* host = nullptr);

    namespace Register {
        /* Menu buttons are the buttons on the left in the main menu */
        /// @brief register a menu button for the left main menu
        /// @param text the text the button will display
        /// @param hoverHint the text that is displayed when the text is hovered
        /// @param onClick what to run when you click the button
        /// @return the created MenuButton*, or nullptr if it failed
        BSML_EXPORT MenuButton* RegisterMenuButton(std::string_view text, std::string_view hoverHint = "", std::function<void(void)> onClick = nullptr);

        /// @brief register a menu button for the left main menu
        /// @param button the button to register
        /// @return true if successful, false if failed
        BSML_EXPORT bool RegisterMenuButton(MenuButton* button);

        /// @brief unregister a menu button for the left main menu
        /// @param button the button to unregister
        /// @return true if successful, false if failed
        BSML_EXPORT bool UnRegisterMenuButton(MenuButton* button);

        /* The settings menus are where the game settings are as well */
        /// @brief register a menu for the settings menu
        /// @param name the name displayed for your settings
        /// @param content_key the key for the BSML content to use for your settings
        /// @param host the host object, used as host for your BSML content
        /// @param enableExtraButtons whether to enable the extra ok / cancel buttons
        /// @return true if successful, false if failed
        BSML_EXPORT bool RegisterSettingsMenu(std::string_view name, std::string_view content_key, System::Object* host, bool enableExtraButtons = false);

        /// @brief register a menu for the settings menu
        /// @param csType the type of your view controller, keep in mind this should inherit HMUI::ViewController*
        /// @param name the name displayed for your settings
        /// @param menuType what kind of source is used for the menu content
        /// @param enableExtraButtons whether to enable the extra ok / cancel buttons
        /// @throw throws if menuType is anything other than ViewController or FlowCoordinator
        /// @return true if successful, false if failed
        BSML_EXPORT bool RegisterSettingsMenu(std::string_view name, System::Type* csType, MenuSource menuSource, bool enableExtraButtons = false);

        /// @brief register a menu for the settings menu
        /// @tparam T the type of your view controller, keep in mind this should inherit HMUI::ViewController*
        /// @param name the name displayed for your settings
        /// @param enableExtraButtons whether to enable the extra ok / cancel buttons
        /// @return true if successful, false if failed
        template<typename T>
        requires(std::is_convertible_v<T, HMUI::ViewController*>)
        static inline bool RegisterSettingsMenu(std::string_view name, bool enableExtraButtons = false) {
            return RegisterSettingsMenu(name, csTypeOf(T), MenuSource::ViewController, enableExtraButtons);
        }

        /// @brief register a menu for the settings menu
        /// @tparam T the type of your flowCoordinator, keep in mind this should inherit HMUI::FlowCoordinator*
        /// @param name the name displayed for your settings
        /// @return true if successful, false if failed
        template<typename T>
        requires(std::is_convertible_v<T, HMUI::FlowCoordinator*>)
        static inline bool RegisterSettingsMenu(std::string_view name) {
            return RegisterSettingsMenu(name, csTypeOf(T), MenuSource::FlowCoordinator, false);
        }

        /// @brief register a menu for the settings menu
        /// @param name the name displayed for your settings
        /// @param viewControllerDidActivate callback ran when the view controller you want to create is opened
        /// @param enableExtraButtons whether to enable the extra ok / cancel buttons
        /// @return true if successful, false if failed
        BSML_EXPORT bool RegisterSettingsMenu(std::string_view name, std::function<void(HMUI::ViewController*, bool, bool, bool)> viewControllerDidActivate, bool enableExtraButtons);

        /// @brief remove a menu from the settings menu
        /// @param host the host object used when registering your menu
        /// @return true if successful, false if failed
        BSML_EXPORT bool UnRegisterSettingsMenu(System::Object* host);

        /* The gameplay tabs are where the song selection menu is */
        /// @brief register a tab for the gameplay setup menu
        /// @param name the name to display
        /// @param content_key the key for the BSML content for your settings
        /// @param host the host object, used as host for your BSML content
        /// @param menuType where your tab should be displayed. Default is All
        /// @return true if successful, false if failed
        BSML_EXPORT bool RegisterGameplaySetupTab(std::string_view name, std::string_view content_key, System::Object* host, MenuType menuType = MenuType::All);

        /// @brief register a tab for the gameplay setup menu
        /// @param csType C# type of your component you want to register, should inherit MonoBehaviour!
        /// @param name the name to display
        /// @param menuType where your tab should be displayed. Default is All
        /// @return true if successful, false if failed
        BSML_EXPORT bool RegisterGameplaySetupTab(System::Type* csType, std::string_view name, MenuType menuType = MenuType::All);

        /// @brief concept to ensure the passed type has a void DidActivate(bool)
        template<typename T>
        concept WithDidActivate = requires(T a) {
            { a->DidActivate(false) } -> std::same_as<void>;
        };

        /// @brief register a tab for the gameplay setup menu
        /// @param csType C# type of your component you want to register
        /// @param name the name to display
        /// @param menuType where your tab should be displayed. Default is All
        /// @return true if successful, false if failed
        template<WithDidActivate T>
        requires(std::is_convertible_v<T, UnityEngine::MonoBehaviour*>)
        static inline bool RegisterGameplaySetupTab(std::string_view name, MenuType menuType = MenuType::All) {
            return RegisterGameplaySetupTab(csTypeOf(T), name, menuType);
        }

        /// @brief register a tab for the gameplay setup menu
        /// @param name the name to display
        /// @param didActivate callback ran when the tab is activated
        /// @param menuType where your tab should be displayed. Default is All
        /// @return true if successful, false if failed
        BSML_EXPORT bool RegisterGameplaySetupTab(std::string_view name, std::function<void(UnityEngine::GameObject*, bool)> didActivate, MenuType menuType = MenuType::All);

        /// @brief remove a tab from the settings menu
        /// @param name the name of the tab to remove
        /// @return true if successful, false if failed
        BSML_EXPORT bool UnRegisterGameplaySetupTab(std::string_view name);

        /// @brief Registers a main menu button that opens a flow coordinator
        /// @param buttonText the text of the button
        /// @param hoverhint the hover hint of the button
        /// @param flowCoordinatorType csType of the flow coordinator
        BSML_EXPORT void RegisterMainMenuFlowCoordinator(const std::string_view& buttonText, const std::string_view& hoverhint, System::Type* flowCoordinatorType);

        /// @brief Registers a main menu button that opens a view controller
        /// @param title the title displayed above the view controller
        /// @param buttonText the text of the button
        /// @param hoverhint the hover hint of the button
        /// @param viewControllerType csType of the flow coordinator
        BSML_EXPORT void RegisterMainMenuViewController(const std::string_view& title, const std::string_view& buttonText, const std::string_view& hoverhint, System::Type* viewControllerType);

        /// @brief Registers a main menu button that opens a view controller
        /// @param title the title displayed above the view controller
        /// @param buttonText the text of the button
        /// @param hoverhint the hover hint of the button
        /// @param viewControllerDidActivate callback ran when the view controller you want to create is opened
        BSML_EXPORT void RegisterMainMenuViewControllerMethod(const std::string_view& title, const std::string_view& buttonText, const std::string_view& hoverhint, std::function<void(HMUI::ViewController*, bool, bool, bool)> viewControllerDidActivate);

        /// @brief Register a view controller type to be displayed when clicking a main menu button
        /// @tparam T the type of the view controller
        /// @param title the title to be displayed above the view controller
        /// @param buttonText the button text
        /// @param hoverhint the hoverhint for the button
        template<typename T>
        requires(std::is_convertible_v<T, HMUI::ViewController*>)
        static inline void RegisterMainMenu(const std::string_view& title, const std::string_view& buttonText, const std::string_view& hoverhint) {
            return RegisterMainMenuViewController(title, buttonText, hoverhint, csTypeOf(T));
        }

        /// @brief Register a flow coordinator type to be displayed when clicking a main menu button
        /// @tparam T the type of the flow coordinator
        /// @param buttonText the button text
        /// @param hoverhint the hoverhint for the button
        template<typename T>
        requires(std::is_convertible_v<T, HMUI::FlowCoordinator*>)
        static inline void RegisterMainMenu(const std::string_view& buttonText, const std::string_view& hoverhint) {
            return RegisterMainMenuFlowCoordinator(buttonText, hoverhint, csTypeOf(T));
        }

        /// @brief Register a callback to run on a view controller when clicking a main menu button
        /// @tparam T your method type
        /// @param title the title to be displayed above the view controller
        /// @param buttonText the button text
        /// @param hoverhint the hoverhint for the button
        /// @param viewControllerDidActivate the callback to execute
        template<typename T>
        requires(std::is_constructible_v<std::function<void(HMUI::ViewController*, bool, bool, bool)>, T>)
        static inline void RegisterMainMenu(const std::string_view& title, const std::string_view& buttonText, const std::string_view& hoverhint, T viewControllerDidActivate = nullptr) {
            return RegisterMainMenuViewControllerMethod(title, buttonText, hoverhint, viewControllerDidActivate);
        }
    }

    namespace Events {
        /// @brief Event that gets invoked right before MenuTransitionsHelper::RestartGame ( Game Restart )
        BSML_EXPORT extern UnorderedEventCallback<> onGameDidRestart;
    }
}
