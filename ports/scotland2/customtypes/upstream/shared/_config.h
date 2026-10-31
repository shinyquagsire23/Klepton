#pragma once

#define CUSTOM_TYPES_EXPORT __attribute__((visibility("default")))
#ifdef __cplusplus
#define CUSTOM_TYPES_FUNC extern "C" CUSTOM_TYPES_EXPORT
#else
#define CUSTOM_TYPES_FUNC CUSTOM_TYPES_EXPORT
#endif
