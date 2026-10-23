/*
 * Nexora OS - ex/worker.c
 * The system worker thread: runs queued work items at PASSIVE_LEVEL, in FIFO order.
 */
#include "ex.h"
#include "ke.h"
#include "ps.h"
#include "rtl.h"

#define EXP_WORKER_PRIORITY (KI_THREAD_PRIORITY_DEFAULT + 4)

static KSPIN_LOCK ExpWorkLock;
static WORK_QUEUE_ITEM *ExpWorkHead;
static WORK_QUEUE_ITEM *ExpWorkTail;
static KEVENT ExpWorkEvent;
static bool ExpWorkerInitialized;

void ExInitializeWorkItem(WORK_QUEUE_ITEM *Item, WORKER_THREAD_ROUTINE Routine, void *Context)
{
    Item->Next = NULL;
    Item->Routine = Routine;
    Item->Context = Context;
    Item->Queued = false;
}

static void ExpEnsureInitialized(void)
{
    /* Work can be queued in phase 0 (before the worker thread exists): set up lazily, with
       interrupts still disabled then, so no race is possible. */
    if (!ExpWorkerInitialized) {
        KeInitializeSpinLock(&ExpWorkLock);
        KeInitializeEvent(&ExpWorkEvent, SynchronizationEvent, false);
        ExpWorkerInitialized = true;
    }
}

void ExQueueWorkItem(WORK_QUEUE_ITEM *Item)
{
    ExpEnsureInitialized();
    KIRQL OldIrql;
    KeAcquireSpinLock(&ExpWorkLock, &OldIrql);
    if (Item->Queued) {
        KeReleaseSpinLock(&ExpWorkLock, OldIrql);
        return;
    }
    Item->Queued = true;
    Item->Next = NULL;
    if (ExpWorkTail)
        ExpWorkTail->Next = Item;
    else
        ExpWorkHead = Item;
    ExpWorkTail = Item;
    KeReleaseSpinLock(&ExpWorkLock, OldIrql);
    KeSetEvent(&ExpWorkEvent);
}

static void ExpWorkerThread(void *Context)
{
    for (;;) {
        KeWaitForSingleObject(&ExpWorkEvent, NULL);
        for (;;) {
            KIRQL OldIrql;
            KeAcquireSpinLock(&ExpWorkLock, &OldIrql);
            WORK_QUEUE_ITEM *Item = ExpWorkHead;
            if (Item) {
                ExpWorkHead = Item->Next;
                if (!ExpWorkHead)
                    ExpWorkTail = NULL;
                /* Cleared before the routine runs, so the routine (or anyone) may queue it again. */
                Item->Queued = false;
            }
            KeReleaseSpinLock(&ExpWorkLock, OldIrql);
            if (!Item)
                break;
            Item->Routine(Item->Context);
        }
    }
}

void ExInitializeWorkerThread(void)
{
    ExpEnsureInitialized();
    STATUS Status = PsCreateSystemThread(ExpWorkerThread, NULL, EXP_WORKER_PRIORITY, "Worker", NULL);
    if (!HZ_SUCCESS(Status))
        KeBugCheckEx(PROCESS_INITIALIZATION_FAILED, PS_INIT_FAILURE_SYSTEM_THREAD, (uint64_t)Status, 1, 0);
    KeSetEvent(&ExpWorkEvent); /* drain anything queued before the thread existed */
}
