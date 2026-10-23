/*
 * Nexora OS - include/ob.h
 * Object manager (Ob*): typed, reference-counted kernel objects, per-process handle tables and
 * the object namespace (directories, symbolic links, parse procedures).
 *
 * Every object is preceded by an OBJECT_HEADER. An object lives while its pointer count is
 * non-zero; each handle also holds a pointer reference. Named objects created with ObCreateObject
 * are permanent: their directory entry holds a reference. Dereferencing is allowed at
 * IRQL <= DISPATCH_LEVEL; a deletion triggered at DISPATCH_LEVEL runs on the worker thread.
 *
 * Namespace (case-insensitive, '\' separated):
 *   \ObjectTypes\<type>      \Device\Ramdisk0       \Driver (M5)
 *   \??\C:  -> symbolic link to \Device\Ramdisk0 in the live session
 * Lookups follow symbolic links and hand the unparsed remainder to the parse procedure of the
 * object they stop at (e.g. a device resolves a file path inside it).
 */
#ifndef OB_H
#define OB_H

#include "hzbase.h"
#include "ke.h"
#include "uapi/hzapi.h"

HZ_EXTERN_C_BEGIN

struct _EPROCESS;
struct _OBJECT_TYPE;

/* Resolves RemainingPath (e.g. "\Nexora\System\hello.exe") inside Object; returns a new
   referenced object in *Result. ParseContext is what the caller of ObReferenceObjectByNameEx
   passed (the I/O manager's open parameters), or NULL. */
typedef STATUS (*OB_PARSE_PROCEDURE)(void *Object, const char *RemainingPath, void *ParseContext, void **Result);
/* Last reference gone: release resources owned by the body (the body itself is freed by Ob). */
typedef void (*OB_DELETE_PROCEDURE)(void *Object);
/* A handle to the object was closed (HandleCount = handles remaining). */
typedef void (*OB_CLOSE_PROCEDURE)(void *Object, struct _EPROCESS *Process, uint64_t HandleCount);

typedef struct _OBJECT_TYPE_INITIALIZER {
    uint32_t PoolTag;
    ACCESS_MASK ValidAccessMask; /* GENERIC_ALL maps to this */
    bool Waitable;               /* body starts with a DISPATCHER_HEADER */
    OB_DELETE_PROCEDURE DeleteProcedure;
    OB_CLOSE_PROCEDURE CloseProcedure;
    OB_PARSE_PROCEDURE ParseProcedure;
} OBJECT_TYPE_INITIALIZER;

typedef struct _OBJECT_TYPE {
    char Name[24];
    OBJECT_TYPE_INITIALIZER Info;
    volatile uint64_t ObjectCount;
    volatile uint64_t HandleCount;
} OBJECT_TYPE;

extern OBJECT_TYPE *ObTypeObjectType;
extern OBJECT_TYPE *ObDirectoryObjectType;
extern OBJECT_TYPE *ObSymbolicLinkObjectType;
extern OBJECT_TYPE *ExEventObjectType;

/* Phase 0: object types, the root directory and the standard directories. */
void ObInitializeSystem(void);
STATUS ObCreateObjectType(const char *Name, const OBJECT_TYPE_INITIALIZER *Initializer, OBJECT_TYPE **Type);

/*
 * Allocates an object with one pointer reference. With a Name (absolute path), the object is
 * also inserted into the namespace as a permanent object.
 */
STATUS ObCreateObject(OBJECT_TYPE *Type, size_t BodySize, const char *Name, void **Object);
void ObReferenceObject(void *Object);
void ObDereferenceObject(void *Object);
OBJECT_TYPE *ObGetObjectType(void *Object);
const char *ObGetObjectName(void *Object); /* NULL if unnamed */
uint64_t ObGetPointerCount(void *Object);
uint64_t ObGetHandleCount(void *Object);

/* ---- Handles ---------------------------------------------------------------------------- */

typedef struct _HANDLE_TABLE HANDLE_TABLE;

HANDLE_TABLE *ObCreateHandleTable(void);
/* Closes every handle and frees the table. */
void ObKillHandleTable(HANDLE_TABLE *Table, struct _EPROCESS *Process);
uint32_t ObHandleCount(HANDLE_TABLE *Table);

/* Creates a handle in Process (current process if NULL); the handle holds its own reference.
   GENERIC_ALL in DesiredAccess maps to the type's ValidAccessMask. */
STATUS ObOpenObjectByPointer(void *Object, ACCESS_MASK DesiredAccess, struct _EPROCESS *Process, HANDLE *Handle);
/*
 * Looks up a handle of the current process and returns the object with a new reference.
 * Type NULL accepts any type. In UserMode the granted access must include DesiredAccess.
 * HZ_CURRENT_PROCESS / HZ_CURRENT_THREAD resolve to the caller's process / thread.
 */
STATUS ObReferenceObjectByHandle(HANDLE Handle, OBJECT_TYPE *Type, ACCESS_MASK DesiredAccess,
                                 KPROCESSOR_MODE Mode, void **Object);
STATUS ObCloseHandle(HANDLE Handle, KPROCESSOR_MODE Mode);

/* ---- Namespace -------------------------------------------------------------------------- */

/* Resolves an absolute path ("\??\C:\Nexora\System\x.exe") to a referenced object. */
STATUS ObReferenceObjectByName(const char *Path, OBJECT_TYPE *Type, void **Object);
/* Same, handing ParseContext to a parse procedure met on the way. */
STATUS ObReferenceObjectByNameEx(const char *Path, OBJECT_TYPE *Type, void *ParseContext, void **Object);
STATUS ObCreateSymbolicLink(const char *LinkPath, const char *TargetPath);
/* Creates a permanent directory (its parent must exist). */
STATUS ObCreateDirectoryObject(const char *Path);
/* Logs the namespace tree (trace level). */
void ObDumpNamespace(void);

/* ---- System services (ob/obsvc.c) ------------------------------------------------------- */

STATUS NtClose(const uint64_t *Args);
STATUS NtWaitForSingleObject(const uint64_t *Args);
STATUS NtWaitForMultipleObjects(const uint64_t *Args);

HZ_EXTERN_C_END

#endif /* OB_H */
