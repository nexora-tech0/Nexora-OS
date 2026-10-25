/*
 * Nexora OS - third_party/stb/compat/string.h
 * The slice of <string.h> the vendored stb headers compile against, declared exactly as
 * rtl/string.c defines it. See stdlib.h in this directory for why it exists.
 */
#ifndef HZ_STB_COMPAT_STRING_H
#define HZ_STB_COMPAT_STRING_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void *memcpy(void *Dest, const void *Src, size_t Count);
void *memmove(void *Dest, const void *Src, size_t Count);
void *memset(void *Dest, int Value, size_t Count);
int memcmp(const void *A, const void *B, size_t Count);
size_t strlen(const char *Str);
size_t strnlen(const char *Str, size_t Max);
int strcmp(const char *A, const char *B);
int strncmp(const char *A, const char *B, size_t Count);

#ifdef __cplusplus
}
#endif

#endif /* HZ_STB_COMPAT_STRING_H */
