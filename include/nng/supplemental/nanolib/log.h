//
// Copyright 2023 NanoMQ Team, Inc. <jaylin@emqx.io> //
// This software is supplied under the terms of the MIT License, a
// copy of which should be located in the distribution where this
// file was obtained (LICENSE.txt).  A copy of the license may also be
// found online at https://opensource.org/licenses/MIT.
//

#ifndef NNG_NANOLIB_LOG_H
#define NNG_NANOLIB_LOG_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include "nng/nng.h"

#define LOG_VERSION "0.2.1"

/**
 * 2023-12-08 move conf_log to log.h
*/

// log type
#define LOG_TO_FILE (1 << 0)
#define LOG_TO_CONSOLE (1 << 1)
#define LOG_TO_SYSLOG (1 << 2)

typedef struct conf_log conf_log;
struct conf_log {
	uint8_t  type;
	int      level;
	char    *dir;
	char    *file;
	FILE    *fp;
	char    *abs_path;        // absolut path of log file
	char    *rotation_sz_str; // 1000KB, 100MB, 10GB
	uint64_t rotation_sz;     // unit: byte
	size_t   rotation_count;  // rotation count
};

typedef struct {
	va_list     ap;
	const char *fmt;
	const char *file;
	const char *func;
	struct tm   time;
	void *      udata;
	int         line;
	int         level;
	conf_log *  config;
} log_event;

typedef void (*log_func)(log_event *ev);

enum {
	NNG_LOG_FATAL = 0,
	NNG_LOG_ERROR,
	NNG_LOG_WARN,
	NNG_LOG_INFO,
	NNG_LOG_DEBUG,
	NNG_LOG_TRACE,
};

NNG_DECL const char *log_level_string(int level);
NNG_DECL int         log_level_num(const char *level);
NNG_DECL void        log_set_level(int level);
NNG_DECL int         log_add_callback(
            log_func fn, void *udata, int level, void *mtx, conf_log *config);
NNG_DECL void log_add_console(int level, void *mtx);
NNG_DECL int  log_add_fp(FILE *fp, int level, void *mtx, conf_log *config);
NNG_DECL void log_add_syslog(const char *log_name, uint8_t level, void *mtx);
NNG_DECL void log_log(int level, const char *file, int line, const char *func,
    const char *fmt, ...);
NNG_DECL void log_clear_callback();

// level: check enum above
// type: 2--> console, 1-->file, 3--> console & file
NNG_DECL int  conf_log_init(int level, int type, char *dir, char *file, uint64_t rotation_sz, size_t rotation_count);
NNG_DECL int  conf_log_fini();

#ifdef ENABLE_LOG

#define log_trace(...) \
    log_log(NNG_LOG_TRACE, __FILE__, __LINE__, __FUNCTION__, __VA_ARGS__)
#define log_debug(...) \
    log_log(NNG_LOG_DEBUG, __FILE__, __LINE__, __FUNCTION__, __VA_ARGS__)
#define log_info(...) \
    log_log(NNG_LOG_INFO, __FILE__, __LINE__, __FUNCTION__, __VA_ARGS__)
#define log_warn(...) \
    log_log(NNG_LOG_WARN, __FILE__, __LINE__, __FUNCTION__, __VA_ARGS__)
#define log_error(...) \
    log_log(NNG_LOG_ERROR, __FILE__, __LINE__, __FUNCTION__, __VA_ARGS__)
#define log_fatal(...) \
    log_log(NNG_LOG_FATAL, __FILE__, __LINE__, __FUNCTION__, __VA_ARGS__)

#else

#define log_trace(...)
#define log_debug(...)
#define log_info(...)
#define log_warn(...)
#define log_error(...)
#define log_fatal(...)

#endif


#ifdef __cplusplus
}
#endif

#endif