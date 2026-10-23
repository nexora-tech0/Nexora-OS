/*
 * Nexora OS - drivers/i8042/i8042.c
 * PS/2 controller (i8042) with keyboard and mouse.
 *
 * The controller is probed directly (some firmware, OVMF included, does not set the 8042 flag
 * in the FADT). Keyboard bytes arrive translated to scan code set 1. Each ISR moves raw bytes
 * into a ring and queues a DPC that decodes them into HZ_KEY_EVENT / HZ_MOUSE_EVENT records
 * (US layout for characters; IntelliMouse 4-byte packets when the mouse has a wheel).
 * \Device\Keyboard0 and \Device\Mouse0 return whole events from IRP_MJ_READ, blocking until one
 * is available (alertably for user-mode callers). win32k will consume the same queues in M6.
 *
 * hz.input=echo starts system threads that read both devices through the I/O manager and log
 * every event (used by the keyboard smoke test).
 */
#include "bootinfo.h"
#include "hal.h"
#include "io.h"
#include "ps.h"
#include "rtl.h"

#define I8042_DATA 0x60
#define I8042_STATUS 0x64
#define I8042_COMMAND 0x64
#define I8042_STATUS_OUTPUT_FULL 0x01
#define I8042_STATUS_INPUT_FULL 0x02
#define I8042_STATUS_AUX 0x20

#define I8042_CONFIG_IRQ1 0x01
#define I8042_CONFIG_IRQ12 0x02
#define I8042_CONFIG_PORT1_CLOCK_OFF 0x10
#define I8042_CONFIG_PORT2_CLOCK_OFF 0x20
#define I8042_CONFIG_TRANSLATE 0x40

#define I8042_IRQL 5
#define PS2_ACK 0xFA
#define PS2_RESEND 0xFE

#define RAW_RING 256
#define EVENT_QUEUE 128

typedef struct _I8042_RING {
    volatile uint8_t Bytes[RAW_RING];
    volatile uint32_t Head; /* written by the ISR */
    volatile uint32_t Tail; /* written by the DPC */
} I8042_RING;

typedef struct _INPUT_QUEUE {
    KSPIN_LOCK Lock;
    KEVENT Available;       /* notification: set while the queue is not empty */
    uint32_t EventSize;
    uint32_t Head, Count;
    uint8_t Events[EVENT_QUEUE][24];
    uint64_t Dropped;
} INPUT_QUEUE;

_Static_assert(sizeof(HZ_KEY_EVENT) <= 24 && sizeof(HZ_MOUSE_EVENT) <= 24, "input event slot size");

static I8042_RING I8042KeyboardRing, I8042MouseRing;
static INPUT_QUEUE I8042KeyboardQueue, I8042MouseQueue;
static KDPC I8042Dpc;
static bool I8042HasMouse, I8042MouseWheel;

/* Decoder state (DPC only) */
static bool KbdExtended;
static int KbdSkip;
static uint16_t KbdModifiers;
static uint8_t MousePacket[4];
static int MouseIndex;

/* ---- Controller access ------------------------------------------------------------------------- */

static bool I8042WaitInputEmpty(void)
{
    for (int i = 0; i < 20000; i++) {
        if (!(HalReadPort8(I8042_STATUS) & I8042_STATUS_INPUT_FULL))
            return true;
        KeStallExecutionProcessor(10);
    }
    return false;
}

static bool I8042Read(uint8_t *Value, uint32_t TimeoutMs)
{
    for (uint32_t i = 0; i < TimeoutMs * 100; i++) {
        if (HalReadPort8(I8042_STATUS) & I8042_STATUS_OUTPUT_FULL) {
            *Value = HalReadPort8(I8042_DATA);
            return true;
        }
        KeStallExecutionProcessor(10);
    }
    return false;
}

static void I8042Command(uint8_t Command)
{
    I8042WaitInputEmpty();
    HalWritePort8(I8042_COMMAND, Command);
}

static void I8042WriteData(uint8_t Value)
{
    I8042WaitInputEmpty();
    HalWritePort8(I8042_DATA, Value);
}

static void I8042Flush(void)
{
    for (int i = 0; i < 64 && (HalReadPort8(I8042_STATUS) & I8042_STATUS_OUTPUT_FULL); i++) {
        (void)HalReadPort8(I8042_DATA);
        KeStallExecutionProcessor(50);
    }
}

/* Sends a byte to the keyboard or the mouse and waits for its acknowledgement. */
static bool I8042SendDevice(bool Mouse, uint8_t Byte)
{
    for (int Attempt = 0; Attempt < 3; Attempt++) {
        if (Mouse)
            I8042Command(0xD4);
        I8042WriteData(Byte);
        uint8_t Reply;
        if (!I8042Read(&Reply, 250))
            return false;
        if (Reply == PS2_ACK)
            return true;
        if (Reply != PS2_RESEND)
            return false;
    }
    return false;
}

/* ---- Event queues ------------------------------------------------------------------------------ */

static void InputQueueInitialize(INPUT_QUEUE *Q, uint32_t EventSize)
{
    KeInitializeSpinLock(&Q->Lock);
    KeInitializeEvent(&Q->Available, NotificationEvent, false);
    Q->EventSize = EventSize;
}

static void InputQueuePush(INPUT_QUEUE *Q, const void *Event)
{
    KIRQL OldIrql;
    KeAcquireSpinLock(&Q->Lock, &OldIrql);
    if (Q->Count == EVENT_QUEUE) {
        Q->Head = (Q->Head + 1) % EVENT_QUEUE; /* drop the oldest */
        Q->Count--;
        Q->Dropped++;
    }
    memcpy(Q->Events[(Q->Head + Q->Count) % EVENT_QUEUE], Event, Q->EventSize);
    Q->Count++;
    KeSetEvent(&Q->Available);
    KeReleaseSpinLock(&Q->Lock, OldIrql);
}

static STATUS InputQueueRead(INPUT_QUEUE *Q, IRP *Irp)
{
    uint32_t Capacity = Irp->Length / Q->EventSize;
    if (Capacity == 0)
        return STATUS_BUFFER_TOO_SMALL;
    for (;;) {
        KIRQL OldIrql;
        KeAcquireSpinLock(&Q->Lock, &OldIrql);
        uint32_t n = HZ_MIN(Capacity, Q->Count);
        for (uint32_t i = 0; i < n; i++) {
            memcpy((uint8_t *)Irp->Buffer + i * Q->EventSize, Q->Events[Q->Head], Q->EventSize);
            Q->Head = (Q->Head + 1) % EVENT_QUEUE;
            Q->Count--;
        }
        if (Q->Count == 0)
            KeResetEvent(&Q->Available);
        KeReleaseSpinLock(&Q->Lock, OldIrql);
        if (n) {
            Irp->IoStatus.Information = n * Q->EventSize;
            return STATUS_SUCCESS;
        }
        void *Object = &Q->Available;
        STATUS Status = KeWaitForMultipleObjectsEx(1, &Object, WaitAny, NULL, NULL, Irp->RequestorMode == UserMode);
        if (Status == STATUS_ALERTED) {
            Irp->IoStatus.Information = 0;
            return STATUS_ALERTED;
        }
    }
}

/* ---- Keyboard decoding (scan code set 1) ------------------------------------------------------- */

static const char KbdNormal[0x3A] = {
    0,   27,  '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b', '\t', 'q', 'w', 'e', 'r',
    't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\r', 0,  'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';',
    '\'', '`', 0,  '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0,   '*', 0,   ' '};
static const char KbdShifted[0x3A] = {
    0,   27,  '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b', '\t', 'Q', 'W', 'E', 'R',
    'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\r', 0,  'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':',
    '"', '~', 0,   '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0,   '*', 0,   ' '};

static uint8_t KbdVirtualKey(uint8_t Scan, bool Extended)
{
    if (Extended) {
        switch (Scan) {
        case 0x1C: return HZ_VK_RETURN;
        case 0x1D: return HZ_VK_CONTROL;
        case 0x35: return HZ_VK_OEM;
        case 0x38: return HZ_VK_MENU;
        case 0x47: return HZ_VK_HOME;
        case 0x48: return HZ_VK_UP;
        case 0x49: return HZ_VK_PRIOR;
        case 0x4B: return HZ_VK_LEFT;
        case 0x4D: return HZ_VK_RIGHT;
        case 0x4F: return HZ_VK_END;
        case 0x50: return HZ_VK_DOWN;
        case 0x51: return HZ_VK_NEXT;
        case 0x52: return HZ_VK_INSERT;
        case 0x53: return HZ_VK_DELETE;
        case 0x5B:
        case 0x5C: return HZ_VK_LWIN;
        default: return 0;
        }
    }
    switch (Scan) {
    case 0x01: return HZ_VK_ESCAPE;
    case 0x0E: return HZ_VK_BACK;
    case 0x0F: return HZ_VK_TAB;
    case 0x1C: return HZ_VK_RETURN;
    case 0x1D: return HZ_VK_CONTROL;
    case 0x2A:
    case 0x36: return HZ_VK_SHIFT;
    case 0x38: return HZ_VK_MENU;
    case 0x39: return HZ_VK_SPACE;
    case 0x3A: return HZ_VK_CAPITAL;
    case 0x45: return HZ_VK_NUMLOCK;
    case 0x46: return HZ_VK_SCROLL;
    case 0x57: return HZ_VK_F1 + 10;
    case 0x58: return HZ_VK_F1 + 11;
    default: break;
    }
    if (Scan >= 0x3B && Scan <= 0x44)
        return (uint8_t)(HZ_VK_F1 + (Scan - 0x3B));
    if (Scan < 0x3A) {
        char c = KbdShifted[Scan];
        if (c >= 'A' && c <= 'Z')
            return (uint8_t)c;
        if (KbdNormal[Scan] >= '0' && KbdNormal[Scan] <= '9')
            return (uint8_t)KbdNormal[Scan];
        if (KbdNormal[Scan])
            return HZ_VK_OEM;
    }
    return 0;
}

static void KbdDecode(uint8_t Byte)
{
    if (KbdSkip > 0) {
        KbdSkip--;
        return;
    }
    if (Byte == 0xE0) {
        KbdExtended = true;
        return;
    }
    if (Byte == 0xE1) {
        KbdSkip = 5; /* Pause/Break: E1 1D 45 E1 9D C5 */
        return;
    }
    if (Byte == PS2_ACK || Byte == PS2_RESEND || Byte == 0xEE || Byte == 0x00 || Byte == 0xFF)
        return; /* controller and keyboard replies, overrun */
    bool Extended = KbdExtended;
    KbdExtended = false;
    bool Break = (Byte & 0x80) != 0;
    uint8_t Scan = Byte & 0x7F;
    if (Extended && (Scan == 0x2A || Scan == 0x36))
        return; /* fake shifts around Print Screen */

    uint16_t Bit = 0;
    if (Scan == 0x2A || Scan == 0x36)
        Bit = HZ_MOD_SHIFT;
    else if (Scan == 0x1D)
        Bit = HZ_MOD_CTRL;
    else if (Scan == 0x38)
        Bit = HZ_MOD_ALT;
    if (Bit)
        KbdModifiers = Break ? (KbdModifiers & ~Bit) : (KbdModifiers | Bit);
    if (!Break && !Extended && Scan == 0x3A)
        KbdModifiers ^= HZ_MOD_CAPS_LOCK;
    if (!Break && !Extended && Scan == 0x45)
        KbdModifiers ^= HZ_MOD_NUM_LOCK;

    HZ_KEY_EVENT Event;
    memset(&Event, 0, sizeof(Event));
    Event.TimeUs = KeQueryUptimeMicroseconds();
    Event.ScanCode = Scan;
    Event.Flags = (Break ? HZ_KEY_BREAK : 0) | (Extended ? HZ_KEY_EXTENDED : 0);
    Event.Modifiers = KbdModifiers;
    Event.VirtualKey = KbdVirtualKey(Scan, Extended);
    if (!Extended && Scan < 0x3A && !(KbdModifiers & (HZ_MOD_CTRL | HZ_MOD_ALT))) {
        bool Shift = (KbdModifiers & HZ_MOD_SHIFT) != 0;
        char c = Shift ? KbdShifted[Scan] : KbdNormal[Scan];
        if ((KbdModifiers & HZ_MOD_CAPS_LOCK) && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')))
            c = Shift ? KbdNormal[Scan] : KbdShifted[Scan];
        Event.Character = (uint8_t)c;
    } else if (Extended && Scan == 0x35) {
        Event.Character = '/';
    } else if (Extended && Scan == 0x1C) {
        Event.Character = '\r';
    } else if (!Extended && (KbdModifiers & HZ_MOD_NUM_LOCK) && Scan >= 0x47 && Scan <= 0x53) {
        static const char Keypad[] = "789-456+1230.";
        Event.Character = (uint8_t)Keypad[Scan - 0x47];
    }
    InputQueuePush(&I8042KeyboardQueue, &Event);
}

static void MouseDecode(uint8_t Byte)
{
    if (MouseIndex == 0 && !(Byte & 0x08))
        return; /* resynchronize: bit 3 is always set in the first byte */
    MousePacket[MouseIndex++] = Byte;
    if (MouseIndex < (I8042MouseWheel ? 4 : 3))
        return;
    MouseIndex = 0;
    uint8_t Flags = MousePacket[0];
    if (Flags & 0xC0)
        return; /* overflow: drop the packet */
    HZ_MOUSE_EVENT Event;
    memset(&Event, 0, sizeof(Event));
    Event.TimeUs = KeQueryUptimeMicroseconds();
    int Dx = MousePacket[1] - ((Flags & 0x10) ? 256 : 0);
    int Dy = MousePacket[2] - ((Flags & 0x20) ? 256 : 0);
    Event.DeltaX = (int16_t)Dx;
    Event.DeltaY = (int16_t)-Dy; /* PS/2 counts up as positive */
    if (I8042MouseWheel) {
        int Z = MousePacket[3] & 0x0F;
        if (Z & 0x08)
            Z -= 16;
        Event.Wheel = (int16_t)-Z;
    }
    Event.Buttons = (Flags & 1 ? HZ_MOUSE_LEFT : 0) | (Flags & 2 ? HZ_MOUSE_RIGHT : 0) | (Flags & 4 ? HZ_MOUSE_MIDDLE : 0);
    InputQueuePush(&I8042MouseQueue, &Event);
}

static void I8042DpcRoutine(KDPC *Dpc, void *Context, void *Arg1, void *Arg2)
{
    while (I8042KeyboardRing.Tail != I8042KeyboardRing.Head) {
        uint8_t Byte = I8042KeyboardRing.Bytes[I8042KeyboardRing.Tail % RAW_RING];
        I8042KeyboardRing.Tail++;
        KbdDecode(Byte);
    }
    while (I8042MouseRing.Tail != I8042MouseRing.Head) {
        uint8_t Byte = I8042MouseRing.Bytes[I8042MouseRing.Tail % RAW_RING];
        I8042MouseRing.Tail++;
        MouseDecode(Byte);
    }
}

static bool I8042Interrupt(KTRAP_FRAME *Frame, void *Context)
{
    bool Handled = false;
    for (int i = 0; i < 16; i++) {
        uint8_t Status = HalReadPort8(I8042_STATUS);
        if (!(Status & I8042_STATUS_OUTPUT_FULL))
            break;
        uint8_t Byte = HalReadPort8(I8042_DATA);
        I8042_RING *Ring = (Status & I8042_STATUS_AUX) ? &I8042MouseRing : &I8042KeyboardRing;
        if (Ring->Head - Ring->Tail < RAW_RING) {
            Ring->Bytes[Ring->Head % RAW_RING] = Byte;
            Ring->Head++;
        }
        Handled = true;
    }
    if (Handled)
        KeInsertQueueDpc(&I8042Dpc, NULL, NULL);
    return Handled;
}

/* ---- Devices --------------------------------------------------------------------------------- */

static STATUS I8042Read8042Device(DEVICE_OBJECT *Device, IRP *Irp)
{
    INPUT_QUEUE *Q = Device->DeviceType == FILE_DEVICE_KEYBOARD ? &I8042KeyboardQueue : &I8042MouseQueue;
    return InputQueueRead(Q, Irp);
}

static void I8042EchoThread(void *Context)
{
    bool Mouse = Context != NULL;
    FILE_OBJECT *File;
    if (!HZ_SUCCESS(IoCreateFile(Mouse ? "\\Device\\Mouse0" : "\\Device\\Keyboard0", FILE_READ_DATA, FILE_OPEN, 0,
                                 KernelMode, &File, NULL)))
        return;
    for (;;) {
        uint8_t Buffer[8 * 24];
        uint32_t Got = 0;
        if (!HZ_SUCCESS(IoReadFile(File, Buffer, Mouse ? 8 * sizeof(HZ_MOUSE_EVENT) : 8 * sizeof(HZ_KEY_EVENT), NULL,
                                   KernelMode, &Got)))
            break;
        if (Mouse) {
            for (uint32_t i = 0; i < Got / sizeof(HZ_MOUSE_EVENT); i++) {
                HZ_MOUSE_EVENT *E = (HZ_MOUSE_EVENT *)Buffer + i;
                DbgPrint("Input: mouse dx=%d dy=%d wheel=%d buttons=0x%x\n", E->DeltaX, E->DeltaY, E->Wheel, E->Buttons);
            }
        } else {
            for (uint32_t i = 0; i < Got / sizeof(HZ_KEY_EVENT); i++) {
                HZ_KEY_EVENT *E = (HZ_KEY_EVENT *)Buffer + i;
                char Shown[4] = "";
                if (E->Character >= 0x20 && E->Character < 0x7F)
                    RtlFormatString(Shown, sizeof(Shown), "'%c'", E->Character);
                DbgPrint("Input: key %s%s (scan 0x%02x%s, VK 0x%02x, modifiers 0x%x)\n", Shown[0] ? Shown : "-",
                         (E->Flags & HZ_KEY_BREAK) ? " up" : " down", E->ScanCode,
                         (E->Flags & HZ_KEY_EXTENDED) ? " E0" : "", E->VirtualKey, E->Modifiers);
            }
        }
    }
    ObDereferenceObject(File);
}

static bool I8042InitializeMouse(void)
{
    uint8_t Reply;
    if (!I8042SendDevice(true, 0xFF)) /* reset */
        return false;
    if (!I8042Read(&Reply, 750) || Reply != 0xAA)
        return false;
    I8042Read(&Reply, 50); /* device ID 0x00 */
    /* IntelliMouse: sample rates 200, 100, 80 enable the wheel; the ID becomes 3. */
    static const uint8_t Rates[] = {200, 100, 80};
    for (size_t i = 0; i < HZ_ARRAYSIZE(Rates); i++) {
        if (!I8042SendDevice(true, 0xF3) || !I8042SendDevice(true, Rates[i]))
            return false;
    }
    if (I8042SendDevice(true, 0xF2) && I8042Read(&Reply, 50))
        I8042MouseWheel = Reply == 3;
    I8042SendDevice(true, 0xF3);
    I8042SendDevice(true, 100); /* 100 samples per second */
    return I8042SendDevice(true, 0xF4); /* enable streaming */
}

static STATUS I8042DriverEntry(DRIVER_OBJECT *Driver)
{
    if (HalReadPort8(I8042_STATUS) == 0xFF)
        return STATUS_NO_SUCH_DEVICE;
    I8042Command(0xAD); /* disable both ports while configuring */
    I8042Command(0xA7);
    I8042Flush();

    uint8_t Config;
    I8042Command(0x20);
    if (!I8042Read(&Config, 50))
        return STATUS_NO_SUCH_DEVICE;
    Config &= (uint8_t) ~(I8042_CONFIG_IRQ1 | I8042_CONFIG_IRQ12);
    Config |= I8042_CONFIG_TRANSLATE;
    I8042Command(0x60);
    I8042WriteData(Config);

    uint8_t Reply;
    I8042Command(0xAA); /* controller self-test */
    if (!I8042Read(&Reply, 500) || Reply != 0x55) {
        DbgPrintEx(DPFLTR_WARNING_LEVEL, "PS/2: controller self-test failed (0x%02x)\n", Reply);
        return STATUS_DEVICE_DATA_ERROR;
    }
    I8042Command(0x60); /* some controllers reset the configuration during the self-test */
    I8042WriteData(Config);

    bool DualChannel = false;
    if (Config & I8042_CONFIG_PORT2_CLOCK_OFF) {
        I8042Command(0xA8);
        I8042Command(0x20);
        uint8_t Check;
        DualChannel = I8042Read(&Check, 50) && !(Check & I8042_CONFIG_PORT2_CLOCK_OFF);
        I8042Command(0xA7);
    }

    I8042Command(0xAE); /* keyboard port on */
    I8042Flush();
    bool Keyboard = I8042SendDevice(false, 0xFF) && I8042Read(&Reply, 750) && Reply == 0xAA;
    Keyboard = Keyboard && I8042SendDevice(false, 0xF4);
    if (DualChannel) {
        I8042Command(0xA8);
        I8042HasMouse = I8042InitializeMouse();
    }
    I8042Flush();

    InputQueueInitialize(&I8042KeyboardQueue, sizeof(HZ_KEY_EVENT));
    InputQueueInitialize(&I8042MouseQueue, sizeof(HZ_MOUSE_EVENT));
    KeInitializeDpc(&I8042Dpc, I8042DpcRoutine, NULL);
    Driver->MajorFunction[IRP_MJ_READ] = I8042Read8042Device;

    DEVICE_OBJECT *Device;
    uint8_t Vector;
    if (Keyboard && HZ_SUCCESS(IoCreateDevice(Driver, 0, "\\Device\\Keyboard0", FILE_DEVICE_KEYBOARD, 0, &Device)) &&
        HZ_SUCCESS(KeConnectInterruptAnyVector(I8042_IRQL, I8042Interrupt, NULL, &Vector)))
        HalEnableIsaInterrupt(1, Vector);
    if (I8042HasMouse && HZ_SUCCESS(IoCreateDevice(Driver, 0, "\\Device\\Mouse0", FILE_DEVICE_MOUSE, 0, &Device)) &&
        HZ_SUCCESS(KeConnectInterruptAnyVector(I8042_IRQL, I8042Interrupt, NULL, &Vector)))
        HalEnableIsaInterrupt(12, Vector);

    /* The configuration byte was read while both ports were disabled: clear the clock-disable
       bits of the ports in use, or this write would switch them off again. */
    Config |= (Keyboard ? I8042_CONFIG_IRQ1 : 0) | (I8042HasMouse ? I8042_CONFIG_IRQ12 : 0);
    if (Keyboard)
        Config &= (uint8_t)~I8042_CONFIG_PORT1_CLOCK_OFF;
    if (I8042HasMouse)
        Config &= (uint8_t)~I8042_CONFIG_PORT2_CLOCK_OFF;
    I8042Command(0x60);
    I8042WriteData(Config);

    DbgPrint("PS/2: keyboard %s, mouse %s\n", Keyboard ? "ready (scan code set 1)" : "not found",
             I8042HasMouse ? (I8042MouseWheel ? "ready (wheel)" : "ready") : DualChannel ? "not found" : "no port");
    if (BiCommandLineHasOption("hz.input=echo")) {
        if (Keyboard)
            PsCreateSystemThread(I8042EchoThread, NULL, KI_THREAD_PRIORITY_DEFAULT + 2, "KeyboardEcho", NULL);
        if (I8042HasMouse)
            PsCreateSystemThread(I8042EchoThread, (void *)1, KI_THREAD_PRIORITY_DEFAULT + 2, "MouseEcho", NULL);
    }
    return Keyboard || I8042HasMouse ? STATUS_SUCCESS : STATUS_NO_SUCH_DEVICE;
}

IO_DECLARE_BUILTIN_DRIVER(I8042BuiltinDriver, "\\Driver\\i8042prt", I8042DriverEntry, IO_ORDER_DEVICE);
