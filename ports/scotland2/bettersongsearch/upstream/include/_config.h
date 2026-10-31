#pragma once

#define BSS_EXPORT __attribute__((visibility("default")))
#ifdef __cplusplus
#define BSS_EXPORT_FUNC extern "C" BSS_EXPORT
#else
#define BSS_EXPORT_FUNC BSS_EXPORT
#endif
