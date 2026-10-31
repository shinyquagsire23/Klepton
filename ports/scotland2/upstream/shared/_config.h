#pragma once

#define MODLOADER_EXPORT __attribute__((visibility("default")))
#ifdef __cplusplus
#define MODLOADER_FUNC extern "C" __attribute__((visibility("default")))
#else
#define MODLOADER_FUNC MODLOADER_EXPORT
#endif
