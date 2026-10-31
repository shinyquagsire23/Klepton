#include "utilities/logging.hpp"
#include "utilities/hooking.hpp"
#include "System/Exception.hpp"

#include "Zenject/ZenjectException.hpp"

MAKE_AUTO_HOOK_MATCH(
    ZenjectException_ctor_1,
    static_cast<void(Zenject::ZenjectException::*)(StringW)>(&::Zenject::ZenjectException::_ctor),
    void,
    Zenject::ZenjectException* self,
    StringW exceptionMessage
) {
    ERROR("ZenjectException: {}", exceptionMessage);
    ZenjectException_ctor_1(self, exceptionMessage);
}

MAKE_AUTO_HOOK_MATCH(
    ZenjectException_ctor_2,
    static_cast<void(Zenject::ZenjectException::*)(StringW, System::Exception*)>(&::Zenject::ZenjectException::_ctor),
    void,
    Zenject::ZenjectException* self,
    StringW exceptionMessage,
    System::Exception* innerException
) {
    ERROR("ZenjectException: {}", innerException->_message->ToString());
    ZenjectException_ctor_2(self, exceptionMessage, innerException);
}
