//
// Copyright 2023 NanoMQ Team, Inc. <jaylin@emqx.io> //
// This software is supplied under the terms of the MIT License, a
// copy of which should be located in the distribution where this
// file was obtained (LICENSE.txt).  A copy of the license may also be
// found online at https://opensource.org/licenses/MIT.
//

#include "nng/supplemental/nanolib/log.h"
#include "nng/supplemental/util/platform.h"
#include "nng/supplemental/nanolib/file.h"
#include "core/nng_impl.h"
#include "core/defs.h"

#define INDEX_FILE_NAME ".idx"

#if defined(SUPP_SYSLOG)
#include <syslog.h>
#endif

#define MAX_CALLBACKS 10

#if NNG_PLATFORM_WINDOWS
#define nano_localtime(t, pTm) localtime_s(pTm, t)
#else
#include <sys/types.h>
#include <unistd.h>
#include <sys/syscall.h>
#define nano_localtime(t, pTm) localtime_r(t, pTm)
#endif

typedef struct {
	log_func  fn;
	void *    udata;
	uint8_t   level;
	nng_mtx * mtx;
	conf_log *config;
} log_callback;

static struct {
	void *       udata;
	uint8_t      level;
	log_callback callbacks[MAX_CALLBACKS];
} L;

static const char *level_strings[] = {
	"FATAL",
	"ERROR",
	"WARN",
	"INFO",
	"DEBUG",
	"TRACE",
};

#ifdef LOG_USE_COLOR
static const char *level_colors[] = {
	"\x1b[35m",
	"\x1b[31m",
	"\x1b[33m",
	"\x1b[32m",
	"\x1b[36m",
	"\x1b[94m",
};
#endif

static void file_rotation(FILE *fp, conf_log *config);

static void
stdout_callback(log_event *ev)
{
	char buf[64];
#if (NNG_PLATFORM_WINDOWS || NNG_PLATFORM_DARWIN)
	int pid = (int) getpid();
#else
	pid_t pid = syscall(__NR_gettid);
#endif

	buf[strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &ev->time)] = '\0';
#ifdef LOG_USE_COLOR
	fprintf(ev->udata,
	    "%s [%i] %s%-5s\x1b[0m \x1b[0m%s:%d \x1b[0m %s: ", buf, pid,
	    level_colors[ev->level], level_strings[ev->level], ev->file,
	    ev->line, ev->func);
#else
	fprintf(ev->udata, "%s [%i] %-5s %s:%d %s: ", buf, pid,
	    level_strings[ev->level], ev->file, ev->line, ev->func);
#endif
	vfprintf(ev->udata, ev->fmt, ev->ap);
	fprintf(ev->udata, "\n");
	fflush(ev->udata);
}

static void
file_callback(log_event *ev)
{
	char buf[64];
#if (NNG_PLATFORM_WINDOWS || NNG_PLATFORM_DARWIN)
	int pid = (int) getpid();
#else
	pid_t pid = syscall(__NR_gettid);
#endif
	FILE *fp = ev->config->fp;
	printf("dir:%s file:%s\n",ev->config->dir, ev->config->file);
	buf[strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &ev->time)] = '\0';
	fprintf(fp, "%s [%i] %-5s %s:%d: ", buf, pid,
	    level_strings[ev->level], ev->file, ev->line);
	vfprintf(fp, ev->fmt, ev->ap);
	fprintf(fp, "\n");
	fflush(fp);

	file_rotation(fp, ev->config);
}

#if defined(SUPP_SYSLOG)

static uint8_t
convert_syslog_level(uint8_t level)
{
	switch (level) {
	case NNG_LOG_FATAL:
		return LOG_EMERG;
	case NNG_LOG_ERROR:
		return LOG_ERR;
	case NNG_LOG_WARN:
		return LOG_WARNING;
	case NNG_LOG_INFO:
		return LOG_INFO;
	case NNG_LOG_DEBUG:
	case NNG_LOG_TRACE:
		return LOG_DEBUG;
	default:
		return LOG_WARNING;
	}
}

static void
syslog_callback(log_event *ev)
{
	vsyslog(ev->level, ev->fmt, ev->ap);
}

void
log_add_syslog(const char *log_name, uint8_t level, void *mtx)
{
	openlog(log_name, LOG_PID, LOG_DAEMON | convert_syslog_level(level));
	log_add_callback(syslog_callback, NULL, level, mtx, NULL);
}
#else

void
log_add_syslog(const char *log_name, uint8_t level, void *mtx)
{
	NNI_ARG_UNUSED(log_name);
	NNI_ARG_UNUSED(level);
	NNI_ARG_UNUSED(mtx);
}

#endif

const char *
log_level_string(int level)
{
	return level_strings[level];
}

int
log_level_num(const char *level)
{
	int count = (int) (sizeof(level_strings) / sizeof(level_strings[0]));
	for (int i = 0; i < count; i++) {
		if (nni_strcasecmp(level, level_strings[i]) == 0) {
			return i;
		}
	}
	return -1;
}

void
log_set_level(int level)
{
	L.level = level;
}

int
log_add_callback(
    log_func fn, void *udata, int level, void *mtx, conf_log *config)
{
	for (int i = 0; i < MAX_CALLBACKS; i++) {
		if (!L.callbacks[i].fn) {
			L.callbacks[i] = (log_callback) {
				.fn     = fn,
				.udata  = udata,
				.level  = level,
				.mtx    = (nng_mtx *) mtx,
				.config = config,
			};
			return 0;
		}
	}
	return -1;
}

void
log_clear_callback()
{
	memset(L.callbacks, 0, sizeof(log_callback) * MAX_CALLBACKS);
}

static void
file_rotation(FILE *fp, conf_log *config)
{
	// Note : do not call log_xxx() in this function, it will cause dead
	// lock
	size_t sz = 0;
	int    rv;
	if ((rv = nni_plat_file_size(config->abs_path, &sz)) != 0) {
		fprintf(stderr, "get file %s size failed: %s\n",
		    config->abs_path, nng_strerror(rv));
		return;
	}

	if (sz >= config->rotation_sz) {
		char *index_file =
		    nano_concat_path(config->dir, INDEX_FILE_NAME);
		char * index_data = NULL;
		size_t size       = 0;
		size_t index      = 1;
		char   buf[4]     = { 0 };

		if ((rv = nni_plat_file_get(
		         index_file, (void **) &index_data, &size)) == 0) {
			memcpy(buf, index_data, size);
			if (1 != sscanf(buf, "%zu", &index)) {
				index = 1;
			}
			nni_free(index_data, size);
		}

		size_t log_name_len = strlen(config->abs_path) + 20;
		char * log_name     = nni_zalloc(log_name_len);
		snprintf(
		    log_name, log_name_len, "%s.%lu", config->file, index);
		char *backup_log_path =
		    nano_concat_path(config->dir, log_name);
		fclose(fp);
		fp = NULL;
		remove(backup_log_path);
		rename(config->abs_path, backup_log_path);
		nni_free(log_name, log_name_len);
		nni_strfree(backup_log_path);

		fp           = fopen(config->abs_path, "a");
		config->fp   = fp;
		char num[20] = { 0 };
		index++; // increase index
		if (index > config->rotation_count) {
			index = 1;
		}
		snprintf(num, 20, "%zu", index);
		if ((rv = nni_plat_file_put(index_file, num, strlen(num))) !=
		    0) {
			fprintf(stderr, "write to file %s failed: %s\n",
			    index_file, nng_strerror(rv));
		}
		nni_strfree(index_file);
	}
}

int
log_add_fp(FILE *fp, int level, void *mtx, conf_log *config)
{
	return log_add_callback(file_callback, fp, level, mtx, config);
}

void
log_add_console(int level, void *mtx)
{
	log_add_callback(stdout_callback, stdout, level, mtx, NULL);
}

static void
init_event(log_event *ev, void *udata, conf_log *config)
{
	const time_t now_seconds = time(NULL);
	nano_localtime(&now_seconds, &ev->time);
	ev->udata  = udata;
	ev->config = config;
}

void
log_log(int level, const char *file, int line, const char *func,
    const char *fmt, ...)
{
	const char *file_name = file;

	log_event ev = {
		.fmt   = fmt,
		.file  = file_name,
		.line  = line,
		.level = level,
		.func  = func,
	};

	for (int i = 0; i < MAX_CALLBACKS && L.callbacks[i].fn; i++) {
		log_callback *cb = &L.callbacks[i];
		if (level <= cb->level) {
			init_event(&ev, cb->udata, cb->config);
			va_start(ev.ap, fmt);
			if (cb->mtx == NULL) {
				cb->fn(&ev);
			} else {
				nng_mtx_lock(cb->mtx);
				cb->fn(&ev);
				nng_mtx_unlock(cb->mtx);
			}
			va_end(ev.ap);
		}
	}
}

static nng_mtx *log_file_mtx = NULL;

static int
log_file_init(conf_log *log)
{
	if (log->dir != NULL && !nni_file_is_dir(log->dir)) {
		log_fatal("%s is not a directory, make sure it's "
		          "created before starting nanomq",
		    log->dir);
		return NNG_EINVAL;
	}
	log->dir   = log->dir == NULL ? nng_strdup("./") : log->dir;
	log->file  = log->file == NULL ? nng_strdup("nanomq_cli.log") : log->file;
	char *path = nano_concat_path(log->dir, log->file);
	log->fp    = fopen(path, "a");
	if (log->fp == NULL) {
		log_fatal("open log file '%s' failed", path);
		nng_strfree(path);
		return NNG_EINVAL;
	}
	log->abs_path = path;
	return 0;
}

static int
log_init(conf_log *log)
{
	int rv = 0;

	log_set_level(log->level);

	if (0 != (log->type & LOG_TO_CONSOLE)) {
		log_add_console(log->level, NULL);
	}

	if (0 != (log->type & LOG_TO_FILE)) {
		if (0 != (rv = log_file_init(log)) ||
		    0 != (rv = nng_mtx_alloc(&log_file_mtx))) {
			return rv;
		}
		log_add_fp(log->fp, log->level, log_file_mtx, log);
	}

	return 0;
}

static int
log_fini(conf_log *log)
{
	if (0 != (log->type & LOG_TO_FILE)) {
		nng_mtx_free(log_file_mtx);
	}

	log_clear_callback();

	return 0;
}

static conf_log *cli_log = NULL;

int
conf_log_init(int level, int type, char *dir, char *file, uint64_t rotation_sz, size_t rotation_count)
{	
	if((cli_log = nng_zalloc(sizeof(conf_log))) == NULL) {
		fprintf(stderr,
		    "Cannot allocate storge for log, quit\n");
		return -1;
	}
	cli_log->level          = level;
	cli_log->type           = type;
	cli_log->dir            = nng_strdup(dir);
	cli_log->file           = nng_strdup(file);
	cli_log->rotation_sz    = rotation_sz;
	cli_log->rotation_count = rotation_count;

	log_init(cli_log);

	return 0;
}

int conf_log_fini()
{
	cli_log->level = NNG_LOG_WARN;
	if (cli_log->fp) {
		fclose(cli_log->fp);
		cli_log->fp = NULL;
	}
	if (cli_log->file) {
		nni_strfree(cli_log->file);
	}
	if (cli_log->dir) {
		nni_strfree(cli_log->dir);
	}
	if (cli_log->rotation_sz_str) {
		nni_strfree(cli_log->rotation_sz_str);
	}
	if (cli_log->abs_path) {
		nni_strfree(cli_log->abs_path);
	}
	cli_log->type           = LOG_TO_CONSOLE;
	cli_log->rotation_count = 5;
	cli_log->rotation_sz    = 10 * 1024;

	log_fini(cli_log);
	nng_free(cli_log, sizeof(cli_log));
	cli_log = NULL;
	return 0;
}
