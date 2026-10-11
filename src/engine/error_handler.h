/*
 * Copyright (c) 2026 General Arcade (Pte. Ltd.)
 * SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-GeneralArcade-Commercial
 */

#ifndef RMMZ_ERROR_HANDLER_H
#define RMMZ_ERROR_HANDLER_H

#include <stdbool.h>
#include <stddef.h>

/* Log levels, ordered by severity. */
typedef enum {
    LOG_DEBUG = 0,
    LOG_INFO  = 1,
    LOG_WARN  = 2,
    LOG_ERROR = 3
} LogLevel;

/* Callback signature for log output capture (used by tests). */
typedef void (*LogCallback)(LogLevel level, const char *message, void *userdata);

/* Initialize logging. log_file_path may be NULL to disable file output;
   messages below min_level are discarded. */
void error_handler_init(const char *log_file_path, LogLevel min_level);

/* Shut down the error handler and close any open log file. */
void error_handler_shutdown(void);

/* Log a message at the given level. Supports printf-style format strings. */
void error_handler_log(LogLevel level, const char *fmt, ...);

/* Set the minimum log level. Messages below this level are discarded. */
void error_handler_set_level(LogLevel min_level);

/* Get the current minimum log level. */
LogLevel error_handler_get_level(void);

/* Set a log callback for capturing output (primarily for testing).
   When set, log output goes to the callback in addition to file/console. */
void error_handler_set_callback(LogCallback cb, void *userdata);

/* Log the message at ERROR level and show an SDL message box (if SDL is up). */
void error_handler_show_error_dialog(const char *title, const char *message);

/* Log the pending JS exception (message + stack), dispatch it to window.onerror,
   and show an error dialog when fatal is true. engine is a JSEngine*. */
void error_handler_report_js_exception(void *engine, bool fatal);

/* Crash recovery: ask the JS side to auto-save. Returns true on success. */
bool error_handler_attempt_auto_save(void *engine);

/* Get the log level name as a string (e.g., "DEBUG", "INFO"). */
const char *error_handler_level_name(LogLevel level);

/* Compatibility log: things the game asked for that this runtime does not do (or does differently), and errors
   the game hit. Each (category, key) pair is logged in full the first time, then counted; the line repeats at
   10, 100, 1000... occurrences and a table of everything is printed every minute and at shutdown. Lines start with
   "[compat]" so a log can be searched for them. `key` identifies the problem (keep it free of ids and numbers that
   change, or every occurrence counts as a new problem); `fmt` is the human detail and may vary. Thread-safe.
   Categories in use: exception, rejection, console-error, console-warn, webgl, canvas2d, render, shader, asset,
   audio, video, api. */
void compat_report(const char *category, const char *key, const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 3, 4)))
#endif
    ;

/* The frame number stamped on new entries (called by the game loop). */
void compat_set_frame(unsigned long long frame);

/* Print the summary table if a minute has passed and something changed since the last one. */
void compat_tick(void);

/* Print the summary table now. */
void compat_summary(void);

#endif /* RMMZ_ERROR_HANDLER_H */
