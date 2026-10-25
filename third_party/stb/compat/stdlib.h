/*
 * Nexora OS - third_party/stb/compat/stdlib.h
 * The slice of <stdlib.h> the vendored stb headers compile against. User programs are built with
 * -nostdinc, so no system <stdlib.h> exists; stb_image.h includes one unconditionally, and this
 * file is what that include finds (the Makefile adds third_party/stb/compat to the user include
 * path). The functions are declared exactly as user/lib/hzrt/heap.cpp defines them.
 */
#ifndef HZ_STB_COMPAT_STDLIB_H
#define HZ_STB_COMPAT_STDLIB_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void *malloc(size_t Size);
void *calloc(size_t Count, size_t Size);
void *realloc(void *Pointer, size_t Size);
void free(void *Pointer);
int abs(int Value);

#ifdef __cplusplus
}
#endif

#endif /* HZ_STB_COMPAT_STDLIB_H */
