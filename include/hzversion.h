/*
 * Nexora OS - include/hzversion.h
 * Product identity. HZ_BUILD_ID comes from build/gen/hzbuildid.h, which the Makefile rewrites
 * only when `git describe` changes (or "local" outside a git checkout).
 */
#ifndef HZVERSION_H
#define HZVERSION_H

#if __has_include("hzbuildid.h")
#include "hzbuildid.h"
#endif

#define HZ_PRODUCT_NAME "Nexora OS"
#define HZ_VERSION_MAJOR 1
#define HZ_VERSION_MINOR 1
#define HZ_VERSION_PATCH 0
#define HZ_VERSION_STRING "1.1.0"
#define HZ_MILESTONE "M12"

#ifndef HZ_BUILD_ID
#define HZ_BUILD_ID "local"
#endif

#endif /* HZVERSION_H */
