/* -*- Mode: C; tab-width: 4; -*- */
/*
* 文件名称：logging.c
* 摘	要：MiniEAP日志功能
* 作	者：updateing@HUST
* 邮	箱：haotia@gmail.com
*/

#include <time.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

#include "logging.h"
#include "minieap_runtime.h"

#define LOG_FORMAT_BUFFER_SIZE 1024
#define DEFAULT_LOG_FILE "/tmp/minieap.log"

/* Long enough for the date line plus wide year values, per -Wformat-truncation */
#define LOG_TIME_BUFFER_SIZE 64

static char g_time_buffer[LOG_TIME_BUFFER_SIZE]; // Buffer for time output
static char* g_log_path = DEFAULT_LOG_FILE; // Log file path
static FILE* g_log_fp = NULL; // Log destination
static LOG_DEST g_dest = LOG_TO_CONSOLE;

/*
 * Serialize writes. The engine runs in a worker thread when embedded in the
 * GUI, and the GUI thread may log too, so concurrent lines must not interleave.
 */
#ifdef _WIN32
#include <windows.h>
static CRITICAL_SECTION g_log_lock;
static volatile LONG g_log_lock_ready = 0;

static void log_lock_init_once() {
	if (InterlockedCompareExchange(&g_log_lock_ready, 1, 0) == 0) {
		InitializeCriticalSection(&g_log_lock);
	}
}

static void log_lock()   { log_lock_init_once(); EnterCriticalSection(&g_log_lock); }
static void log_unlock() { LeaveCriticalSection(&g_log_lock); }
#else
#include <pthread.h>
static pthread_mutex_t g_log_lock = PTHREAD_MUTEX_INITIALIZER;

static void log_lock()   { pthread_mutex_lock(&g_log_lock); }
static void log_unlock() { pthread_mutex_unlock(&g_log_lock); }
#endif

static char* get_formatted_date() {
	time_t time_tmp;
	struct tm* time_s;

	time(&time_tmp);
	time_s = localtime(&time_tmp);

	sprintf(g_time_buffer, "%d/%d/%d %d:%02d:%02d", time_s->tm_year + 1900, time_s->tm_mon + 1,
				time_s->tm_mday, time_s->tm_hour, time_s->tm_min, time_s->tm_sec);
	return g_time_buffer;
}

/*
 * Strip ANSI escape sequences ("\033[1m", "\033[0m" ...) in place.
 * Keeps host UIs (the Qt GUI) free of terminal codes.
 */
static void strip_ansi(char* s) {
	char* dst = s;
	while (*s) {
		if (s[0] == '\033' && s[1] == '[') {
			s += 2;
			while (*s && (*s == ';' || (*s >= '0' && *s <= '9'))) s++;
			if (*s) s++; /* the terminating letter */
		} else {
			*dst++ = *s++;
		}
	}
	*dst = 0;
}

/*
 * 打印一行文本
 *
 * Every line is forwarded to the host hook (ANSI codes stripped, trailing
 * newline removed), and then written to the current destination as well.
 */
static void print_raw_line(FILE* log_file, LOG_TAG tag, const char* log_format, va_list argptr) {
	char _raw_buf[LOG_FORMAT_BUFFER_SIZE];
	va_list _hook_args, _file_args;
	int _len;

	va_copy(_hook_args, argptr);
	vsnprintf(_raw_buf, sizeof(_raw_buf), log_format, _hook_args);
	va_end(_hook_args);

	/* Drop trailing CR/LF so each hook call is exactly one line */
	_len = (int)strlen(_raw_buf);
	while (_len > 0 && (_raw_buf[_len - 1] == '\n' || _raw_buf[_len - 1] == '\r')) {
		_raw_buf[--_len] = 0;
	}

	log_lock();
	/* Only the host copy is cleaned up; the console still gets the colours */
	strip_ansi(_raw_buf);
	_len = (int)strlen(_raw_buf);
	if (g_minieap_log_hook != NULL) {
		g_minieap_log_hook(tag, _raw_buf, _len);
	}

	if (log_file == NULL) {
		fprintf(stderr, "Internal error: Log destination is not set, printing to stdout!\n");
		log_file = stdout;
	}
	va_copy(_file_args, argptr);
	vfprintf(log_file, log_format, _file_args);
	va_end(_file_args);
	log_unlock();
}

/*
 * 打印一行日志并附加时间与调用点信息
 */
static void print_detail_line(FILE* log_file, LOG_TAG tag, const char* log_level,
                      const char* func_name, const char* log_format, va_list argptr) {
	char format_buffer[LOG_FORMAT_BUFFER_SIZE];
    int _strlen;

    if (func_name != NULL && func_name[0] != 0) {
	    snprintf(format_buffer, LOG_FORMAT_BUFFER_SIZE, "[%s][%s](%s) %s",
	            get_formatted_date(), log_level, func_name, log_format);
	} else {
	    snprintf(format_buffer, LOG_FORMAT_BUFFER_SIZE, "[%s][%s] %s",
	             get_formatted_date(), log_level, log_format);
	}

	/* Append a newline if not exist */
	_strlen = strlen(format_buffer);
	if (_strlen > 0 && format_buffer[_strlen - 1] != '\n') {
	    format_buffer[_strlen] = '\n';
	    format_buffer[_strlen + 1] = 0;
	}

	print_raw_line(log_file, tag, format_buffer, argptr);
}

/*
 * 设置日志的目标，是打印到标准输出还是写入文件
 *
 * 注：写入文件时，将直接打开文件来写入，而不是reopen stdout到文件。
 * 故仍可以使用printf来直接打印到控制台（用户交互使用）
 */
void set_log_destination(LOG_DEST dst) {
    g_dest = dst;
}

/*
 * 按之前的目标设置来打印一行日志
 */
void print_log(const char* log_level, const char* func_name, const char* log_format, ...) {
	va_list argptr;
	LOG_TAG _tag = LOG_TAG_INFO;

	if (log_level != NULL) {
		switch (log_level[0]) {
			case 'E': _tag = LOG_TAG_ERROR; break;
			case 'W': _tag = LOG_TAG_WARN; break;
			case 'D': _tag = LOG_TAG_DEBUG; break;
			default:  _tag = LOG_TAG_INFO; break;
		}
	}

	va_start(argptr, log_format);
	print_detail_line(g_log_fp, _tag, log_level, func_name, log_format, argptr);
	va_end(argptr);
}

/*
 * 按之前的目标设置来打印一行文本，不添加时间标记
 */
void print_log_raw(const char* log_format, ...) {
	va_list argptr;

	va_start(argptr, log_format);
	print_raw_line(g_log_fp, LOG_TAG_NONE, log_format, argptr);
	va_end(argptr);
}

void start_log() {
	switch (g_dest) {
		case LOG_TO_CONSOLE:
			g_log_fp = stdout;
			break;
		case LOG_TO_FILE:
			g_log_fp = fopen(g_log_path, "a");
			if (g_log_fp == NULL) {
			    g_log_fp = stdout;
			    g_dest = LOG_TO_CONSOLE;
			    setvbuf(g_log_fp, NULL, _IOLBF, BUFSIZ);
			    PR_ERRNO("日志文件打开失败，将输出至控制台");
			}
			break;
	}
	if (g_log_fp != NULL) {
		setvbuf(g_log_fp, NULL, _IOLBF, BUFSIZ);
	}
}

void close_log() {
    if (g_dest == LOG_TO_FILE && g_log_fp != NULL && g_log_fp != stdout && g_log_fp != stderr) {
        fclose(g_log_fp);
    }
    g_log_fp = NULL;
}

void set_log_file_path(char* path) {
    g_log_path = path;
}
