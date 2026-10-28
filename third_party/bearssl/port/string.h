#ifndef NEXORA_BEARSSL_STRING_H
#define NEXORA_BEARSSL_STRING_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void *memcpy(void *destination, const void *source, size_t length);
void *memmove(void *destination, const void *source, size_t length);
void *memset(void *destination, int value, size_t length);
int memcmp(const void *left, const void *right, size_t length);
size_t strlen(const char *text);

#ifdef __cplusplus
}
#endif

#endif
