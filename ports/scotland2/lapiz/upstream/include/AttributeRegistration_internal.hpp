#include "AttributeRegistration.hpp"

namespace Lapiz::Attributes {
    /// @brief gets an attribute registrations for a specific class and name
    /// @param klass the klass to look in for attribute registrations
    /// @param name the name of the registration (field / methodname)
    /// @return registration if found, otherwise nullptr
    const AttributeRegistration* Get(Il2CppClass* klass, StringW name);
}
