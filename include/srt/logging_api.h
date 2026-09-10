/* SPDX-License-Identifier: MIT */

#ifndef ROBOTWEAX_SRT_COMPAT_LOGGING_API_H
#define ROBOTWEAX_SRT_COMPAT_LOGGING_API_H

/*
 * The SRT logging API uses the conventional syslog severity numbers on every
 * platform.  Define the small public subset locally on platforms that do not
 * provide <syslog.h> (notably Windows).
 */
#if !defined(_WIN32)
#  include <syslog.h>
#endif

#ifndef LOG_CRIT
#  define LOG_CRIT 2
#endif
#ifndef LOG_ERR
#  define LOG_ERR 3
#endif
#ifndef LOG_WARNING
#  define LOG_WARNING 4
#endif
#ifndef LOG_NOTICE
#  define LOG_NOTICE 5
#endif
#ifndef LOG_INFO
#  define LOG_INFO 6
#endif
#ifndef LOG_DEBUG
#  define LOG_DEBUG 7
#endif
#ifndef LOG_DEBUG_TRACE
#  define LOG_DEBUG_TRACE 8
#endif

#define SRT_LOG_LEVEL_MIN LOG_CRIT
#define SRT_LOG_LEVEL_MAX LOG_DEBUG

#ifdef __cplusplus
/** C++ source-compatible names for the public syslog severity levels. */
namespace srt_logging {
namespace LogLevel {
enum type {
    fatal = LOG_CRIT,
    error = LOG_ERR,
    warning = LOG_WARNING,
    note = LOG_NOTICE,
    debug = LOG_DEBUG
};
} // namespace LogLevel
} // namespace srt_logging
#endif

#define SRT_LOGF_DISABLE_TIME 1
#define SRT_LOGF_DISABLE_THREADNAME 2
#define SRT_LOGF_DISABLE_SEVERITY 4
#define SRT_LOGF_DISABLE_EOL 8

/**
 * @brief Process-wide SRT log callback.
 *
 * The callback executes synchronously on whichever application or SRT worker
 * thread emitted the entry. All string pointers are library-owned and remain
 * valid only until the callback returns; copy anything that must outlive the
 * invocation. Calls that emit another SRT log entry from this callback are
 * suppressed to prevent recursion. The callback must return promptly and must
 * not throw through the C ABI. Its `opaque` context must remain alive until
 * the handler is replaced or cleared with srt_setloghandler().
 *
 * @param opaque Application context registered with srt_setloghandler().
 * @param level Syslog-compatible severity (`LOG_CRIT` through `LOG_DEBUG`).
 * @param file NUL-terminated source location, possibly an empty string.
 * @param line One-based source line, or 0 when unavailable.
 * @param area NUL-terminated functional-area name, possibly empty.
 * @param message NUL-terminated formatted message; newline presence follows
 * `SRT_LOGF_DISABLE_EOL`.
 */
typedef void SRT_LOG_HANDLER_FN(
    void* opaque,
    int level,
    const char* file,
    int line,
    const char* area,
    const char* message);

#endif
