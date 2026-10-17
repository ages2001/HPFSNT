/*
 * toolcrt.h - force-included prefix for building the HPFSNT tools freestanding
 * (see toolcrt.c). Provides the few C-library types and declarations the tools
 * need, so their <stdio.h>/<stdlib.h>/<string.h>/<ctype.h>/<time.h> can be
 * blocked (those pull in CRT-only macros like stderr/toupper). <windows.h> is
 * still used normally for the Win32 API.
 */
#ifndef HP_TOOLCRT_H
#define HP_TOOLCRT_H

#ifndef NULL
#define NULL ((void*)0)
#endif
#ifndef _TIME_T_DEFINED
#define _TIME_T_DEFINED
#ifdef _WIN64
typedef long long time_t;
#else
typedef long time_t;
#endif
#endif

struct tm {
    int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year,
        tm_wday, tm_yday, tm_isdst;
};

/* stdio-ish */
extern void *stdout, *stderr;
int   hp_printf(const char *, ...);
int   hp_fprintf(void *, const char *, ...);
int   hp_sprintf(char *, const char *, ...);
int   hp_snprintf(char *, unsigned, const char *, ...);
int   hp_vsnprintf(char *, unsigned, const char *, __builtin_va_list);

/* stdlib / string / ctype / time */
void *hp_malloc(unsigned);
void  hp_free(void *);
void *hp_calloc(unsigned, unsigned);
void *hp_realloc(void *, unsigned);
unsigned long hp_strtoul(const char *, char **, int);
time_t hp_time(time_t *);
struct tm *hp_localtime(const time_t *);
int   hp_toupper(int);
int   hp_stricmp(const char *, const char *);
int   tolower(int), isdigit(int), isalpha(int), isspace(int);
void *memset(void *, int, unsigned);
void *memcpy(void *, const void *, unsigned);
void *memmove(void *, const void *, unsigned);
int   memcmp(const void *, const void *, unsigned);
unsigned strlen(const char *);
int   strcmp(const char *, const char *);
int   strncmp(const char *, const char *, unsigned);
char *strcpy(char *, const char *);
char *strncpy(char *, const char *, unsigned);
char *strchr(const char *, int);

#endif
