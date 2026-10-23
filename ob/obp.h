/*
 * Nexora OS - ob/obp.h
 * Object-manager internals shared by the files in ob/.
 *
 * Memory layout of an object: [OBJECT_HEADER (64 bytes)][body]. The header is never visible
 * outside Ob; every public API takes and returns body pointers.
 *
 * Locks: ObpNamespaceLock protects every directory's entry list and the Name/Directory fields of
 * headers; each handle table has its own spinlock. Neither is held while calling a type's
 * procedures, so procedures may use Ob freely.
 */
#ifndef OBP_H
#define OBP_H

#include "ex.h"
#include "ke.h"
#include "mm.h"
#include "ob.h"
#include "ps.h"
#include "rtl.h"

#define OBP_HEADER_SIGNATURE HZ_POOL_TAG('O', 'b', 'H', 'd')
#define OBP_FREED_SIGNATURE HZ_POOL_TAG('O', 'b', 'X', 'x')
#define OBP_NAME_TAG HZ_POOL_TAG('O', 'b', 'N', 'm')
#define OBP_ENTRY_TAG HZ_POOL_TAG('O', 'b', 'D', 'e')
#define OBP_HANDLE_TAG HZ_POOL_TAG('O', 'b', 'H', 't')

#define OBP_FLAG_PERMANENT 0x1 /* held by its directory entry */

#define OBP_MAX_NAME 64   /* one path component, including the terminator */
#define OBP_MAX_PATH 256  /* whole path, including the terminator */
#define OBP_MAX_REPARSE 16

typedef struct _OBJECT_HEADER {
    volatile int64_t PointerCount;
    volatile int64_t HandleCount;
    OBJECT_TYPE *Type;
    char *Name;                          /* leaf name (pool), NULL if unnamed */
    struct _OBJECT_DIRECTORY *Directory; /* parent directory while named */
    struct _OBJECT_HEADER *NextDeferred; /* deferred-delete list */
    uint32_t Flags;
    uint32_t Signature;
    uint64_t Reserved;
} OBJECT_HEADER;

_Static_assert(sizeof(OBJECT_HEADER) == 64, "object bodies must stay 16-byte aligned");

static inline OBJECT_HEADER *ObpHeader(void *Object)
{
    return (OBJECT_HEADER *)((uint8_t *)Object - sizeof(OBJECT_HEADER));
}

static inline void *ObpBody(OBJECT_HEADER *Header)
{
    return (uint8_t *)Header + sizeof(OBJECT_HEADER);
}

typedef struct _OBJECT_DIRECTORY_ENTRY {
    struct _OBJECT_DIRECTORY_ENTRY *Next;
    void *Object;
} OBJECT_DIRECTORY_ENTRY;

typedef struct _OBJECT_DIRECTORY {
    OBJECT_DIRECTORY_ENTRY *Entries; /* sorted by name, case-insensitive */
    uint32_t Count;
} OBJECT_DIRECTORY;

typedef struct _OBJECT_SYMBOLIC_LINK {
    char *Target; /* absolute path (pool) */
} OBJECT_SYMBOLIC_LINK;

extern KSPIN_LOCK ObpNamespaceLock;
extern OBJECT_DIRECTORY *ObpRootDirectory;

/* ob/object.c */
void ObpCheckHeader(OBJECT_HEADER *Header);
/* Allocates header + body without inserting it anywhere. */
STATUS ObpAllocateObject(OBJECT_TYPE *Type, size_t BodySize, void **Object);
void ObpInitializeDeferredDelete(void);

/* ob/namespace.c */
/* Inserts Object under Path (absolute); the directory entry takes a reference. */
STATUS ObpInsertObjectByName(void *Object, const char *Path);
void ObpInitializeNamespace(void);

/* Case-insensitive comparison of a component (length Length) with a NUL-terminated name. */
int ObpCompareName(const char *Component, size_t Length, const char *Name);

#endif /* OBP_H */
