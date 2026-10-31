#pragma once

#include "../_config.h"
#include "TemplateRedecoratorRegistration.hpp"
#include "Zenject/DiContainer.hpp"
namespace Lapiz::Objects {
    template<typename TPrefab, typename TParent>
    requires(std::is_convertible_v<TPrefab, System::Object*> && std::is_convertible_v<TParent, System::Object*>)
    struct LAPIZ_EXPORT Registration {
        public:
            using Self = Registration<TPrefab, TParent>;
            Registration(const Self& other) = delete;
            Registration(const Self&& other) {
                v = other.v;
                other.v = nullptr;
            }

            Self* operator->() { return this; }
            operator TemplateRedecoratorRegistration* () const { return v; }

            explicit Registration(void* v) : v((TemplateRedecoratorRegistration*)v) {};
            explicit operator void*() const { return v; };

            void RegisterRedecorator(::Zenject::DiContainer* container) {
                container->get_AncestorContainers()[0]->Bind<::Lapiz::Objects::RedecoratorRegistration*>()->FromInstance(v)->AsCached();
            }

            static inline Self New_ctor(std::string_view fieldName, std::function<TPrefab(TPrefab)> redecorateCall, int priority = 0, bool chain = true) {
                return Self(fieldName, redecorateCall, priority, chain);
            }
        private:
            TemplateRedecoratorRegistration* v;

            Registration(std::string_view fieldName, std::function<TPrefab(TPrefab)> redecorateCall, int priority = 0, bool chain = true) {
                v = ::Lapiz::Objects::TemplateRedecoratorRegistration::Make<TPrefab, TParent>(fieldName, redecorateCall, priority, chain);
            }
    };
}

#ifdef LAPIZ_REDECORATION_REGISTRATION_HELPER_DEFINITION
#error "LAPIZ_REDECORATION_REGISTRATION_HELPER_DEFINITION is already defined! Undefine it before including RegistrationHelper.hpp!"
#endif

#define LAPIZ_REDECORATION_REGISTRATION_HELPER_DEFINITION(name_, field_, TPrefab_, TParent_)                                     \
struct LAPIZ_EXPORT name_ {                                                                                                                   \
    using TPrefab = TPrefab_;                                                                                                    \
    using TParent = TParent_;                                                                                                    \
    using Registration = ::Lapiz::Objects::Registration<TPrefab, TParent>;                                                       \
    static inline Registration New_ctor(std::function<TPrefab(TPrefab)> redecorateCall, int priority = 0, bool chain = true) {   \
        return Registration::New_ctor(field_, redecorateCall, priority, chain);                                                  \
    }                                                                                                                            \
}
