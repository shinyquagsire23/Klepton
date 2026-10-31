#pragma once

#define SONGDETAILS_EXPORT __attribute__((visibility("default")))
#ifdef __cplusplus
#define SONGDETAILS_FUNC extern "C" SONGDETAILS_EXPORT
#else
#define SONGDETAILS_FUNC SONGDETAILS_EXPORT
#endif
