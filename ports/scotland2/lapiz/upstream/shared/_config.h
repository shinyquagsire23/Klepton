#pragma once

#define LAPIZ_EXPORT __attribute__((visibility("default")))
#ifdef __cplusplus
#define LAPIZ_EXPORT_FUNC extern "C" LAPIZ_EXPORT
#else
#define LAPIZ_EXPORT_FUNC LAPIZ_EXPORT
#endif
