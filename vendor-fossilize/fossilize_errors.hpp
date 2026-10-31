/* Copyright (c) 2018-2019 Hans-Kristian Arntzen
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#pragma once

#include <string>
#include <utility>
#include <stdint.h>
#include <atomic>
#include <string.h>
#include "fossilize_inttypes.h"

#if defined(_MSC_VER) && (_MSC_VER <= 1800)
#define FOSSILIZE_NOEXCEPT
#else
#define FOSSILIZE_NOEXCEPT noexcept
#endif

namespace Fossilize
{
// Sets the global logging level for the current thread.
// Used by the Fossilize API.
// Any internal threads created by Fossilize will inherit the log level and callbacks from creating thread.
// FOSSILIZE_API_DEFAULT_LOG_LEVEL define can be used to set initial value.
void set_thread_log_level(LogLevel level);
LogLevel get_thread_log_level();

// Custom callback. Log level filter is somewhat moot, but at least avoids some redundant
// work from happening to build a message that is ignored.
using LogCallback = void (*)(LogLevel level, const char *message, void *userdata);
void set_thread_log_callback(LogCallback cb, void *userdata);

namespace Internal
{
bool log_thread_callback(LogLevel level, const char *fmt, ...);
LogCallback get_thread_log_callback();
void *get_thread_log_userdata();

static inline const char *extract_basepath(const char *path)
{
	const char *base = strrchr(path, '\\');
	if (base)
		return base + 1;

	base = strrchr(path, '/');
	if (base)
		return base + 1;

	return "";
}
}

#ifndef LOGE
#error "LOGE must be defined before including fossilize_errors.hpp."
#endif

#ifndef LOGW
#error "LOGW must be defined before including fossilize_errors.hpp."
#endif

#ifndef FOSSILIZE_SPAM_LIMIT_WARN
#define FOSSILIZE_SPAM_LIMIT_WARN 3
#endif

#ifndef FOSSILIZE_SPAM_LIMIT_ERR
#define FOSSILIZE_SPAM_LIMIT_ERR 3
#endif

#define LOGE_LEVEL(...) do { \
	if (get_thread_log_level() <= LOG_ERROR) { \
		static std::atomic<uint32_t> log_spam_counter; \
		uint32_t spam_count = log_spam_counter.load(std::memory_order_relaxed); \
		if (spam_count <= FOSSILIZE_SPAM_LIMIT_ERR) \
			spam_count = log_spam_counter.fetch_add(1, std::memory_order_relaxed); \
		if (spam_count < FOSSILIZE_SPAM_LIMIT_ERR) { \
			if (!Internal::log_thread_callback(LOG_ERROR, __VA_ARGS__)) { \
				LOGE(__VA_ARGS__); \
			} \
		} else if (spam_count == FOSSILIZE_SPAM_LIMIT_ERR) { \
			const char *basepath_name = Internal::extract_basepath(__FILE__); \
			if (!Internal::log_thread_callback(LOG_ERROR, " ... error spam detected at %s:%d, silencing.\n", basepath_name, __LINE__)) { \
				LOGW(" ... error spam detected at %s:%d, silencing.\n", basepath_name, __LINE__); \
			} \
		} \
	} \
} while(0)

#define LOGW_LEVEL(...) do { \
	if (get_thread_log_level() <= LOG_WARNING) { \
		static std::atomic<uint32_t> log_spam_counter; \
		uint32_t spam_count = log_spam_counter.load(std::memory_order_relaxed); \
		if (spam_count <= FOSSILIZE_SPAM_LIMIT_WARN) \
			spam_count = log_spam_counter.fetch_add(1, std::memory_order_relaxed); \
		if (spam_count < FOSSILIZE_SPAM_LIMIT_WARN) { \
			if (!Internal::log_thread_callback(LOG_WARNING, __VA_ARGS__)) { \
				LOGW(__VA_ARGS__); \
			} \
		} else if (spam_count == FOSSILIZE_SPAM_LIMIT_WARN) { \
			const char *basepath_name = Internal::extract_basepath(__FILE__); \
			if (!Internal::log_thread_callback(LOG_WARNING, " ... warning spam detected at %s:%d, silencing.\n", basepath_name, __LINE__)) { \
				LOGW(" ... warning spam detected at %s:%d, silencing.\n", basepath_name, __LINE__); \
			} \
		} \
	} \
} while(0)

static inline void log_error_pnext_chain(std::string what, const void *pNext)
{
	what += " (pNext->sType chain: [";
	while (pNext != nullptr)
	{
		auto *next = static_cast<const VkBaseInStructure *>(pNext);
		what += std::to_string(next->sType);
		pNext = next->pNext;
		if (pNext != nullptr)
			what += ", ";
	}
	what += "])";
	LOGW_LEVEL("%s\n", what.c_str());
}

static inline void log_missing_resource(const char *type, Hash hash)
{
	LOGW_LEVEL("Referenced %s %016" PRIx64
			           ", but it does not exist.\n"
			           "This can be expected when replaying an archive from Steam.\n"
			           "If replaying just the application cache, "
			           "make sure to replay together with the common cache, "
			           "as application cache can depend on common cache.\n",
	           type, hash);
}

static inline void log_invalid_resource(const char *type, Hash hash)
{
	LOGW_LEVEL("Referenced %s %016" PRIx64
			           ", but it is VK_NULL_HANDLE.\n"
			           "The create info was likely not supported by device.\n",
	           type, hash);
}

template <typename T>
static inline void log_failed_hash(const char *type, T object)
{
	LOGW_LEVEL("%s handle 0x%016" PRIx64
			           " is not registered.\n"
			           "It has either not been recorded, or it failed to be recorded earlier "
			           "(which is expected if application uses an extension that is not recognized by Fossilize).\n",
	           type, (uint64_t)object);
}
}

