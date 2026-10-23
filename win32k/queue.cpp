/*
 * Nexora OS - win32k/queue.cpp
 * Per-thread message queues and timers.
 *
 * Posted and input messages wait in one FIFO per thread; consecutive mouse moves for the same
 * window are coalesced into the latest one, and a full queue (W32_MAX_QUEUED_MESSAGES) drops new
 * posts. WM_PAINT and WM_TIMER are generated when the queue is examined, NT-style: paint while a
 * visible window of the thread has a non-empty update region (BeginPaint validates it), timers
 * when due. GetMessage waits alertably on the thread's event with the earliest timer as timeout,
 * so a thread blocked in it can still be terminated.
 */
#include "w32p.h"

static bool WmMatches(const HZ_MSG *Msg, HWND Filter, uint32_t First, uint32_t Last)
{
    if (Filter && Msg->Window != Filter)
        return false;
    if (First == 0 && Last == 0)
        return true;
    return Msg->Message >= First && Msg->Message <= Last;
}

void WmPostThreadMessage(W32THREAD *Thread, HWND Window, uint32_t Message, uint64_t WParam, uint64_t LParam)
{
    if (!Thread)
        return;
    /* Coalesce mouse moves: replace a pending one for the same window at the end of the queue. */
    if (Message == HZ_WM_MOUSEMOVE && !IsListEmpty(&Thread->Messages)) {
        QUEUED_MESSAGE *Last = CONTAINING_RECORD(Thread->Messages.Blink, QUEUED_MESSAGE, Link);
        if (Last->Msg.Message == HZ_WM_MOUSEMOVE && Last->Msg.Window == Window) {
            Last->Msg.WParam = WParam;
            Last->Msg.LParam = LParam;
            Last->Msg.TimeUs = KeQueryUptimeMicroseconds();
            Last->Msg.Cursor = InpGetCursor();
            KeSetEvent(&Thread->Wake);
            return;
        }
    }
    if (Thread->MessageCount >= W32_MAX_QUEUED_MESSAGES)
        return;
    QUEUED_MESSAGE *Q = (QUEUED_MESSAGE *)ExAllocatePoolWithTag(NonPagedPool, sizeof(QUEUED_MESSAGE), W32_TAG);
    if (!Q)
        return;
    Q->Msg.Window = Window;
    Q->Msg.Message = Message;
    Q->Msg.WParam = WParam;
    Q->Msg.LParam = LParam;
    Q->Msg.TimeUs = KeQueryUptimeMicroseconds();
    Q->Msg.Cursor = InpGetCursor();
    InsertTailList(&Thread->Messages, &Q->Link);
    Thread->MessageCount++;
    KeSetEvent(&Thread->Wake);
}

void WmPostMessage(WINDOW *Window, uint32_t Message, uint64_t WParam, uint64_t LParam)
{
    WmPostThreadMessage(Window->Owner, Window->Handle, Message, WParam, LParam);
}

void WmPurgeMessages(W32THREAD *Thread, HWND Window)
{
    for (LIST_ENTRY *E = Thread->Messages.Flink; E != &Thread->Messages;) {
        QUEUED_MESSAGE *Q = CONTAINING_RECORD(E, QUEUED_MESSAGE, Link);
        E = E->Flink;
        if (Window == 0 || Q->Msg.Window == Window) {
            RemoveEntryList(&Q->Link);
            Thread->MessageCount--;
            ExFreePoolWithTag(Q, W32_TAG);
        }
    }
}

/* ---- Timers -------------------------------------------------------------------------------------- */

STATUS WmSetTimer(WINDOW *Window, uint64_t Id, uint32_t PeriodMs)
{
    if (PeriodMs == 0)
        return STATUS_INVALID_PARAMETER;
    uint64_t Now = KeQueryUptimeMicroseconds();
    for (LIST_ENTRY *E = Window->Timers.Flink; E != &Window->Timers; E = E->Flink) {
        W32TIMER *T = CONTAINING_RECORD(E, W32TIMER, Link);
        if (T->Id == Id) { /* replace */
            T->PeriodUs = (uint64_t)PeriodMs * 1000;
            T->DueUs = Now + T->PeriodUs;
            KeSetEvent(&Window->Owner->Wake);
            return STATUS_SUCCESS;
        }
    }
    W32TIMER *T = (W32TIMER *)ExAllocatePoolZero(NonPagedPool, sizeof(W32TIMER), W32_TAG);
    if (!T)
        return STATUS_NO_MEMORY;
    T->Id = Id;
    T->PeriodUs = (uint64_t)PeriodMs * 1000;
    T->DueUs = Now + T->PeriodUs;
    InsertTailList(&Window->Timers, &T->Link);
    KeSetEvent(&Window->Owner->Wake); /* the waiter recomputes its timeout */
    return STATUS_SUCCESS;
}

STATUS WmKillTimer(WINDOW *Window, uint64_t Id)
{
    for (LIST_ENTRY *E = Window->Timers.Flink; E != &Window->Timers; E = E->Flink) {
        W32TIMER *T = CONTAINING_RECORD(E, W32TIMER, Link);
        if (T->Id == Id) {
            RemoveEntryList(&T->Link);
            ExFreePoolWithTag(T, W32_TAG);
            return STATUS_SUCCESS;
        }
    }
    return STATUS_NOT_FOUND;
}

void WmFreeTimers(WINDOW *Window)
{
    while (!IsListEmpty(&Window->Timers)) {
        W32TIMER *T = CONTAINING_RECORD(RemoveHeadList(&Window->Timers), W32TIMER, Link);
        ExFreePoolWithTag(T, W32_TAG);
    }
}

/* ---- Retrieval ----------------------------------------------------------------------------------- */

/* One examination of the queue (WmLock held). Returns true with *Msg filled, or false with the
   earliest timer due time in *NextDueUs (UINT64_MAX if none). */
static bool WmFindMessage(W32THREAD *T, HZ_MSG *Msg, HWND Filter, uint32_t First, uint32_t Last, bool Remove,
                          uint64_t *NextDueUs)
{
    *NextDueUs = UINT64_MAX;
    for (LIST_ENTRY *E = T->Messages.Flink; E != &T->Messages; E = E->Flink) {
        QUEUED_MESSAGE *Q = CONTAINING_RECORD(E, QUEUED_MESSAGE, Link);
        if (!WmMatches(&Q->Msg, Filter, First, Last))
            continue;
        *Msg = Q->Msg;
        if (Remove) {
            RemoveEntryList(&Q->Link);
            T->MessageCount--;
            ExFreePoolWithTag(Q, W32_TAG);
        }
        return true;
    }
    if (T->QuitPosted && Filter == 0) {
        memset(Msg, 0, sizeof(*Msg));
        Msg->Message = HZ_WM_QUIT;
        Msg->WParam = (uint64_t)T->QuitCode;
        Msg->TimeUs = KeQueryUptimeMicroseconds();
        if (Remove)
            T->QuitPosted = false;
        return true;
    }
    uint64_t Now = KeQueryUptimeMicroseconds();
    for (LIST_ENTRY *E = T->Windows.Flink; E != &T->Windows; E = E->Flink) {
        WINDOW *W = CONTAINING_RECORD(E, WINDOW, ThreadLink);
        if (Filter && W->Handle != Filter)
            continue;
        HZ_MSG Paint = {W->Handle, HZ_WM_PAINT, 0, 0, Now, InpGetCursor()};
        if (W->Visible && !RgnIsEmpty(W->Update) && WmMatches(&Paint, Filter, First, Last)) {
            *Msg = Paint;
            return true;
        }
    }
    for (LIST_ENTRY *E = T->Windows.Flink; E != &T->Windows; E = E->Flink) {
        WINDOW *W = CONTAINING_RECORD(E, WINDOW, ThreadLink);
        if (Filter && W->Handle != Filter)
            continue;
        for (LIST_ENTRY *L = W->Timers.Flink; L != &W->Timers; L = L->Flink) {
            W32TIMER *Timer = CONTAINING_RECORD(L, W32TIMER, Link);
            HZ_MSG Tick = {W->Handle, HZ_WM_TIMER, Timer->Id, 0, Now, InpGetCursor()};
            if (!WmMatches(&Tick, Filter, First, Last))
                continue;
            if (Timer->DueUs <= Now) {
                if (Remove) /* periods missed while busy are skipped, not accumulated */
                    Timer->DueUs = HZ_MAX(Timer->DueUs + Timer->PeriodUs, Now + Timer->PeriodUs / 2);
                *Msg = Tick;
                return true;
            }
            *NextDueUs = HZ_MIN(*NextDueUs, Timer->DueUs);
        }
    }
    return false;
}

STATUS WmGetMessage(HZ_MSG *Msg, HWND Filter, uint32_t First, uint32_t Last, bool Remove, bool Wait)
{
    for (;;) {
        WmAcquire();
        W32THREAD *T = WmGetThread(true);
        if (!T) {
            WmRelease();
            return STATUS_NO_MEMORY;
        }
        uint64_t NextDue;
        bool Found = WmFindMessage(T, Msg, Filter, First, Last, Remove, &NextDue);
        WmRelease();
        if (Found)
            return STATUS_SUCCESS;
        if (!Wait)
            return STATUS_NO_MORE_ENTRIES;
        uint64_t Now = KeQueryUptimeMicroseconds();
        uint64_t Timeout = NextDue == UINT64_MAX ? 0 : (NextDue > Now ? NextDue - Now : 0);
        void *Object = &T->Wake;
        STATUS Status = KeWaitForMultipleObjectsEx(1, &Object, WaitAny, NextDue == UINT64_MAX ? NULL : &Timeout, NULL,
                                                   true);
        if (Status == STATUS_ALERTED)
            return STATUS_ALERTED;
    }
}
