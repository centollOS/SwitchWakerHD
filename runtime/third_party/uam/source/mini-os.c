#include <stdio.h>
#include <stdlib.h>
#include "util/os_misc.h"
#include "uam_log.h"

// SwitchWakerHD patches 3 and 4: the log callback and the fatal-error jump (see uam_log.h); patch 10: per thread
static _Thread_local uam_log_fn s_log_fn;
static _Thread_local void* s_log_user;
__thread jmp_buf* uam_fatal_jmp;

void uam_set_log_callback(uam_log_fn fn, void* user)
{
	s_log_fn = fn;
	s_log_user = user;
}

void uam_vlogf(const char* fmt, va_list ap)
{
	char buf[4096];
	vsnprintf(buf, sizeof(buf), fmt, ap);
	if (s_log_fn)
		s_log_fn(buf, s_log_user);
	else
		fputs(buf, stderr);
}

void uam_logf(const char* fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	uam_vlogf(fmt, ap);
	va_end(ap);
}

void uam_fatal(const char* fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	uam_vlogf(fmt, ap);
	va_end(ap);
	if (uam_fatal_jmp)
		longjmp(*uam_fatal_jmp, 1);
	abort();
}

void os_log_message(const char *message)
{
	uam_logf("%s", message);
}

const char* os_get_option(const char *name)
{
	return NULL;
}
