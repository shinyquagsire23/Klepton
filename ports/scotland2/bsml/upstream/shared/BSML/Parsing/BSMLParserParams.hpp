#pragma once

#include "../../_config.h"
#include "beatsaber-hook/shared/utils/typedefs.h"

#include "UnityEngine/GameObject.hpp"
#include "BSMLValue.hpp"
#include "BSMLAction.hpp"
#include "BSMLEvent.hpp"

#include "System/Action.hpp"

#include <map>
#include <vector>
#include <string>

namespace BSML {
    class BSML_EXPORT BSMLParserParams {
        friend class BSMLTag;
        friend class BSMLMacro;
        friend class BSMLParser;
        public:
            ~BSMLParserParams();
            /// @brief get the object vector for the given tag
            /// @param tag the tag to look for
            /// @return const reference to the vector
            const std::vector<UnityEngine::GameObject*>& GetObjectsWithTag(const std::string& tag) const;

            /// @brief add object with tag to the backing map;
            /// @param object the object to add
            /// @param tag the tag to add the object to
            void AddObjectWithTag(UnityEngine::GameObject* object, const std::string& tag);

            /// @brief add multiple objects with tag to the backing map;
            /// @param objects the objects to add
            /// @param tag the tag to add the objects to
            void AddObjectsWithTag(const std::vector<UnityEngine::GameObject*>& objects, const std::string& tag);

            /// @brief add an object to multiple tags in the backing map
            /// @param object the object to add
            /// @param tags the tags to add the object to
            void AddObjectWithTags(UnityEngine::GameObject* object, const std::vector<std::string>& tags);

            /// @brief pass the tagged objects from this into the other params;
            /// @param other the parser params to pass the objects into
            void PassTaggedObjects(BSMLParserParams& other) const;

            /// @brief Equivalent to getting a BSMLValue from PC
            /// @param key the key the value is saved to
            BSMLValue* TryGetValue(const std::string& key) const;

            /// @brief adds the BSMLValue to the map
            /// @param key the key the value is saved to
            /// @param value the value to save
            void AddValue(const std::string& key, BSMLValue* value);

            /// @brief Equivalent to getting a BSMLAction from PC
            /// @param key the key the action is saved to
            BSMLAction* TryGetAction(const std::string& key) const;

            /// @brief adds the BSMLValue to the map
            /// @param key the key the value is saved to
            /// @param value the value to save
            void AddAction(const std::string& key, BSMLAction* action);

            /// @brief Gets a weak ptr for the event you are looking for
            /// @param key the key for the event
            /// @return weak ptr for event, this will always be a valid weak ptr, but check if it is expired before using it
            std::weak_ptr<BSMLEvent> GetEvent(const std::string& key);

            /// @brief runs the functions for the passed event
            /// @param key the key to invoke
            void EmitEvent(const std::string& key);

            /// @brief adds the BSMLEvent to the map
            /// @param key the key the event is saved to
            /// @param event the function to run for the specific event
            void AddEvent(const std::string& key, std::function<void(void)> event);

            /// @brief get the host object
            System::Object* get_host();
        private:
            std::map<std::string, std::vector<UnityEngine::GameObject*>> objectsWithIds;
            std::map<std::string, BSMLValue*> values;
            std::map<std::string, BSMLAction*> actions;
            std::map<std::string, std::shared_ptr<BSMLEvent>> events;
            System::Object* host;
    };
}
