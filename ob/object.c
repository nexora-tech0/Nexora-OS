/*
 * Nexora OS - ob/object.c
 * Object types, allocation, reference counting and deletion.
 *
 * The last ObDereferenceObject deletes the object: the type's delete procedure releases what
 * the body owns, then Ob frees the name and the memory. Deletion may need to block or touch
 * page tables, so a last reference dropped at DISPATCH_LEVEL (the thread reaper runs there) is
 * queued to the system worker thread instead.
 */
#include "obp.h"

#define OBP_TYPE_TAG HZ_POOL_TAG('O', 'b', 'T', 'y')

OBJECT_TYPE *ObTypeObjectType;
OBJECT_TYPE *ObDirectoryObjectType;
OBJECT_TYPE *ObSymbolicLinkObjectType;

static KSPIN_LOCK ObpDeferredLock;
static OBJECT_HEADER *ObpDeferredList;
static WORK_QUEUE_ITEM ObpDeferredWorkItem;

void ObpCheckHeader(OBJECT_HEADER *Header)
{
    if (Header->Signature != OBP_HEADER_SIGNATURE || Header->PointerCount < 0 || Header->HandleCount < 0)
        KeBugCheckEx(OBJECT_REFCOUNT_CORRUPT, (uint64_t)ObpBody(Header), (uint64_t)Header->PointerCount,
                     (uint64_t)Header->HandleCount, Header->Signature);
}

STATUS ObpAllocateObject(OBJECT_TYPE *Type, size_t BodySize, void **Object)
{
    uint32_t Tag = Type ? Type->Info.PoolTag : OBP_TYPE_TAG;
    OBJECT_HEADER *Header = ExAllocatePoolZero(NonPagedPool, sizeof(OBJECT_HEADER) + BodySize, Tag);
    if (!Header)
        return STATUS_NO_MEMORY;
    Header->PointerCount = 1;
    Header->Type = Type;
    Header->Signature = OBP_HEADER_SIGNATURE;
    if (Type)
        __atomic_add_fetch(&Type->ObjectCount, 1, __ATOMIC_RELAXED);
    *Object = ObpBody(Header);
    return STATUS_SUCCESS;
}

STATUS ObCreateObjectType(const char *Name, const OBJECT_TYPE_INITIALIZER *Initializer, OBJECT_TYPE **Out)
{
    OBJECT_TYPE *Type;
    STATUS Status = ObpAllocateObject(ObTypeObjectType, sizeof(OBJECT_TYPE), (void **)&Type);
    if (!HZ_SUCCESS(Status))
        return Status;
    if (!ObTypeObjectType) {
        /* Bootstrap: the "Type" type is an instance of itself. */
        ObTypeObjectType = Type;
        ObpHeader(Type)->Type = Type;
        Type->ObjectCount = 1;
    }
    RtlStringCopy(Type->Name, sizeof(Type->Name), Name);
    Type->Info = *Initializer;
    if (!Type->Info.ValidAccessMask)
        Type->Info.ValidAccessMask = GENERIC_ALL;

    /* Named \ObjectTypes\<Name> once the namespace exists (the bootstrap types are inserted by
       ObpInitializeNamespace). Type objects are permanent. */
    if (ObpRootDirectory) {
        char Path[OBP_MAX_PATH];
        RtlFormatString(Path, sizeof(Path), "\\ObjectTypes\\%s", Name);
        Status = ObpInsertObjectByName(Type, Path);
        if (!HZ_SUCCESS(Status))
            return Status; /* leaks the type, which only happens on a duplicate name at boot */
        ObDereferenceObject(Type);
    }
    *Out = Type;
    return STATUS_SUCCESS;
}

STATUS ObCreateObject(OBJECT_TYPE *Type, size_t BodySize, const char *Name, void **Object)
{
    if (!Type)
        return STATUS_INVALID_PARAMETER;
    void *Body;
    STATUS Status = ObpAllocateObject(Type, BodySize, &Body);
    if (!HZ_SUCCESS(Status))
        return Status;
    if (Name) {
        Status = ObpInsertObjectByName(Body, Name);
        if (!HZ_SUCCESS(Status)) {
            ObDereferenceObject(Body);
            return Status;
        }
    }
    *Object = Body;
    return STATUS_SUCCESS;
}

void ObReferenceObject(void *Object)
{
    OBJECT_HEADER *Header = ObpHeader(Object);
    int64_t New = __atomic_add_fetch(&Header->PointerCount, 1, __ATOMIC_RELAXED);
    if (New <= 1 || Header->Signature != OBP_HEADER_SIGNATURE)
        KeBugCheckEx(OBJECT_REFCOUNT_CORRUPT, (uint64_t)Object, (uint64_t)New, (uint64_t)Header->HandleCount, 1);
}

static void ObpDeleteObject(OBJECT_HEADER *Header)
{
    void *Body = ObpBody(Header);
    OBJECT_TYPE *Type = Header->Type;
    if (Header->HandleCount != 0 || Header->Directory)
        KeBugCheckEx(OBJECT_REFCOUNT_CORRUPT, (uint64_t)Body, 0, (uint64_t)Header->HandleCount, 2);
    if (Type->Info.DeleteProcedure)
        Type->Info.DeleteProcedure(Body);
    __atomic_sub_fetch(&Type->ObjectCount, 1, __ATOMIC_RELAXED);
    if (Header->Name)
        ExFreePoolWithTag(Header->Name, OBP_NAME_TAG);
    Header->Signature = OBP_FREED_SIGNATURE;
    ExFreePoolWithTag(Header, Type->Info.PoolTag);
}

static void ObpDeferredDeleteWorker(void *Context)
{
    for (;;) {
        KIRQL OldIrql;
        KeAcquireSpinLock(&ObpDeferredLock, &OldIrql);
        OBJECT_HEADER *List = ObpDeferredList;
        ObpDeferredList = NULL;
        KeReleaseSpinLock(&ObpDeferredLock, OldIrql);
        if (!List)
            return;
        while (List) {
            OBJECT_HEADER *Next = List->NextDeferred;
            ObpDeleteObject(List);
            List = Next;
        }
    }
}

void ObpInitializeDeferredDelete(void)
{
    KeInitializeSpinLock(&ObpDeferredLock);
    ExInitializeWorkItem(&ObpDeferredWorkItem, ObpDeferredDeleteWorker, NULL);
}

void ObDereferenceObject(void *Object)
{
    OBJECT_HEADER *Header = ObpHeader(Object);
    if (Header->Signature != OBP_HEADER_SIGNATURE)
        KeBugCheckEx(OBJECT_REFCOUNT_CORRUPT, (uint64_t)Object, (uint64_t)Header->PointerCount, Header->Signature, 3);
    int64_t Remaining = __atomic_sub_fetch(&Header->PointerCount, 1, __ATOMIC_ACQ_REL);
    if (Remaining > 0)
        return;
    if (Remaining < 0)
        KeBugCheckEx(OBJECT_REFCOUNT_CORRUPT, (uint64_t)Object, (uint64_t)Remaining, (uint64_t)Header->HandleCount, 4);

    if (KeGetCurrentIrql() < DISPATCH_LEVEL) {
        ObpDeleteObject(Header);
        return;
    }
    KIRQL OldIrql;
    KeAcquireSpinLock(&ObpDeferredLock, &OldIrql);
    Header->NextDeferred = ObpDeferredList;
    ObpDeferredList = Header;
    KeReleaseSpinLock(&ObpDeferredLock, OldIrql);
    ExQueueWorkItem(&ObpDeferredWorkItem);
}

OBJECT_TYPE *ObGetObjectType(void *Object)
{
    return ObpHeader(Object)->Type;
}

const char *ObGetObjectName(void *Object)
{
    return ObpHeader(Object)->Name;
}

uint64_t ObGetPointerCount(void *Object)
{
    return (uint64_t)ObpHeader(Object)->PointerCount;
}

uint64_t ObGetHandleCount(void *Object)
{
    return (uint64_t)ObpHeader(Object)->HandleCount;
}

/* ---- Directory and symbolic-link types ------------------------------------------------------ */

static void ObpDeleteSymbolicLink(void *Object)
{
    OBJECT_SYMBOLIC_LINK *Link = Object;
    if (Link->Target)
        ExFreePoolWithTag(Link->Target, OBP_NAME_TAG);
}

void ObInitializeSystem(void)
{
    ObpInitializeDeferredDelete();
    KeInitializeSpinLock(&ObpNamespaceLock);

    OBJECT_TYPE_INITIALIZER Init = {0};
    OBJECT_TYPE *Unused;
    Init.PoolTag = OBP_TYPE_TAG;
    if (!HZ_SUCCESS(ObCreateObjectType("Type", &Init, &Unused)))
        KeBugCheckEx(PROCESS_INITIALIZATION_FAILED, PS_INIT_FAILURE_OBJECT_TYPES, 0, 0, 0);

    Init.PoolTag = HZ_POOL_TAG('O', 'b', 'D', 'i');
    if (!HZ_SUCCESS(ObCreateObjectType("Directory", &Init, &ObDirectoryObjectType)))
        KeBugCheckEx(PROCESS_INITIALIZATION_FAILED, PS_INIT_FAILURE_OBJECT_TYPES, 1, 0, 0);

    Init.PoolTag = HZ_POOL_TAG('O', 'b', 'S', 'l');
    Init.DeleteProcedure = ObpDeleteSymbolicLink;
    if (!HZ_SUCCESS(ObCreateObjectType("SymbolicLink", &Init, &ObSymbolicLinkObjectType)))
        KeBugCheckEx(PROCESS_INITIALIZATION_FAILED, PS_INIT_FAILURE_OBJECT_TYPES, 2, 0, 0);

    ObpInitializeNamespace();
}
