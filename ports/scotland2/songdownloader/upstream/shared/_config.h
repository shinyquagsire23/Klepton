#pragma once

#define SONGDOWNLOADER_EXPORT __attribute__((visibility("default")))
#ifdef __cplusplus
#define SONGDOWNLOADER_EXPORT_FUNC extern "C" SONGDOWNLOADER_EXPORT
#else
#define SONGDOWNLOADER_EXPORT_FUNC SONGDOWNLOADER_EXPORT
#endif
