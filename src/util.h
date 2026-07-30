#pragma once
typedef unsigned long size_t;

void  *memcpy(void *dst, const void *src, size_t n);
void  *memset(void *dst, int c, size_t n);
int    memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
char  *strcpy(char *dst, const char *src);
/* decimal-format v into out (no NUL); returns the number of chars written */
int    utoa(unsigned long v, char *out);
/* 1 if s starts with p */
int    str_has_prefix(const char *s, const char *p);
/* lowercase hex, no 0x prefix; returns chars written */
int    xtoa(unsigned long v, char *out);
