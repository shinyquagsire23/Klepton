
#pragma once

#ifdef DECLARE_INJECT_METHOD
#error "DECLARE_INJECT_METHOD already defined! Undefine it before including macros.hpp!"
#endif

#ifdef DECLARE_INJECT_FIELD
#error "DECLARE_INJECT_FIELD already defined! Undefine it before including macros.hpp!"
#endif

#define DECLARE_INJECT_METHOD(ret, name_, ...) \

#define DECLARE_INJECT_FIELD(type_, name_) \
