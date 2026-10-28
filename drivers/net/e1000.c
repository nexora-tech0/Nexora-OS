/*
 * Nexora OS - drivers/net/e1000.c
 * Polled Intel 82540EM (PCI 8086:100E) and 82574L (PCI 8086:10D3) Ethernet controllers. The
 * protocol stack owns polling and packet scheduling; this driver initializes hardware and moves
 * bounded Ethernet frames through DMA rings.
 */
#include "ex.h"
#include "hal.h"
#include "io.h"
#include "ke.h"
#include "mm.h"
#include "netif.h"
#include "pci.h"
#include "rtl.h"

#define E1000_TAG HZ_POOL_TAG('E', '1', 'K', ' ')
#define E1000_VENDOR_ID 0x8086
#define E1000_DEVICE_ID_82540EM 0x100E
#define E1000_DEVICE_ID_82574L 0x10D3
#define E1000_DEVICE_ID_82579LM 0x1502
#define E1000_82579_SUBVENDOR 0x1028
#define E1000_82579_SUBDEVICE 0x052C
#define E1000_82579_REVISION 0x04
#define E1000_RING_COUNT 64
#define E1000_BUFFER_SIZE 2048
#define E1000_RING_PAGES 1
#define E1000_BUFFER_PAGES ((E1000_RING_COUNT * E1000_BUFFER_SIZE) / PAGE_SIZE)
#define E1000_DMA_MAX 0x100000000ull
#define E1000_REG_CTRL 0x0000
#define E1000_REG_STATUS 0x0008
#define E1000_REG_EERD 0x0014
#define E1000_REG_IMC 0x00D8
#define E1000_REG_RCTL 0x0100
#define E1000_REG_TCTL 0x0400
#define E1000_REG_TIPG 0x0410
#define E1000_REG_RDBAL 0x2800
#define E1000_REG_RDBAH 0x2804
#define E1000_REG_RDLEN 0x2808
#define E1000_REG_RDH 0x2810
#define E1000_REG_RDT 0x2818
#define E1000_REG_TDBAL 0x3800
#define E1000_REG_TDBAH 0x3804
#define E1000_REG_TDLEN 0x3808
#define E1000_REG_TDH 0x3810
#define E1000_REG_TDT 0x3818
#define E1000_REG_RAL 0x5400
#define E1000_REG_RAH 0x5404
#define E1000_CTRL_RST (1u << 26)
#define E1000_CTRL_SLU (1u << 6)
#define E1000_STATUS_LINK_UP (1u << 1)
#define E1000_RCTL_EN (1u << 1)
#define E1000_RCTL_BAM (1u << 15)
#define E1000_RCTL_SECRC (1u << 26)
#define E1000_TCTL_EN (1u << 1)
#define E1000_TCTL_PSP (1u << 3)
#define E1000_TX_STATUS_DD (1u << 0)
#define E1000_RX_STATUS_DD (1u << 0)
#define E1000_RX_STATUS_EOP (1u << 1)
#define E1000_TX_CMD_EOP (1u << 0)
#define E1000_TX_CMD_IFCS (1u << 1)
#define E1000_TX_CMD_RS (1u << 3)

typedef struct HZ_PACKED _E1000_TX_DESCRIPTOR {
    uint64_t Address;
    uint16_t Length;
    uint8_t ChecksumOffset;
    uint8_t Command;
    uint8_t Status;
    uint8_t ChecksumStart;
    uint16_t Special;
} E1000_TX_DESCRIPTOR;

typedef struct HZ_PACKED _E1000_RX_DESCRIPTOR {
    uint64_t Address;
    uint16_t Length;
    uint16_t Checksum;
    uint8_t Status;
    uint8_t Errors;
    uint16_t Special;
} E1000_RX_DESCRIPTOR;

typedef struct _E1000_ADAPTER {
    PCI_DEVICE *Pci;
    volatile uint8_t *Registers;
    PFN_NUMBER RxDmaPfn;
    PFN_NUMBER TxDmaPfn;
    volatile E1000_RX_DESCRIPTOR *RxRing;
    volatile E1000_TX_DESCRIPTOR *TxRing;
    uint8_t *RxBuffers;
    uint8_t *TxBuffers;
    uint32_t RxIndex;
    uint32_t TxIndex;
    KSPIN_LOCK RingLock;
    NET_INTERFACE Interface;
} E1000_ADAPTER;

static uint32_t E1000Read(E1000_ADAPTER *Adapter, uint32_t Register)
{
    return *(volatile uint32_t *)(Adapter->Registers + Register);
}

static void E1000Write(E1000_ADAPTER *Adapter, uint32_t Register, uint32_t Value)
{
    *(volatile uint32_t *)(Adapter->Registers + Register) = Value;
}

static bool E1000WaitRegister(E1000_ADAPTER *Adapter, uint32_t Register, uint32_t Mask, uint32_t Value,
                              uint32_t TimeoutUs)
{
    for (uint32_t Elapsed = 0; Elapsed < TimeoutUs; Elapsed += 10) {
        if ((E1000Read(Adapter, Register) & Mask) == Value)
            return true;
        HalStallExecution(10);
    }
    return (E1000Read(Adapter, Register) & Mask) == Value;
}

static bool E1000ReadEeprom(E1000_ADAPTER *Adapter, uint8_t Address, uint16_t *Value)
{
    E1000Write(Adapter, E1000_REG_EERD, 1u | ((uint32_t)Address << 8));
    for (uint32_t Attempt = 0; Attempt < 1000; Attempt++) {
        uint32_t Result = E1000Read(Adapter, E1000_REG_EERD);
        if (Result & (1u << 4)) {
            *Value = (uint16_t)(Result >> 16);
            return true;
        }
        HalStallExecution(10);
    }
    return false;
}

static bool E1000ValidMac(const uint8_t Mac[6])
{
    return !(Mac[0] & 1) && (Mac[0] | Mac[1] | Mac[2] | Mac[3] | Mac[4] | Mac[5]) != 0;
}

static bool E1000ReadMac(E1000_ADAPTER *Adapter, uint8_t Mac[6])
{
    /* The 82574L exposes its NVM-loaded address in RAL/RAH after reset. Its EERD behavior is not
       identical to the 82540EM, so consume that initialized address first for this MAC. */
    if (Adapter->Pci->DeviceId == E1000_DEVICE_ID_82574L ||
        Adapter->Pci->DeviceId == E1000_DEVICE_ID_82579LM) {
        uint32_t Low = E1000Read(Adapter, E1000_REG_RAL);
        uint32_t High = E1000Read(Adapter, E1000_REG_RAH);
        Mac[0] = (uint8_t)Low;
        Mac[1] = (uint8_t)(Low >> 8);
        Mac[2] = (uint8_t)(Low >> 16);
        Mac[3] = (uint8_t)(Low >> 24);
        Mac[4] = (uint8_t)High;
        Mac[5] = (uint8_t)(High >> 8);
        if ((High & (1u << 31)) && E1000ValidMac(Mac))
            return true;
        DbgPrintEx(DPFLTR_WARNING_LEVEL, "E1000: %04x address registers invalid (RAL=%08x RAH=%08x), trying EEPROM\n",
                   Adapter->Pci->DeviceId,
                   Low, High);
    }

    for (uint8_t i = 0; i < 3; i++) {
        uint16_t Word;
        if (!E1000ReadEeprom(Adapter, i, &Word))
        {
            DbgPrintEx(DPFLTR_WARNING_LEVEL, "E1000: EEPROM read timed out at word %u (EERD=%08x)\n", i,
                       E1000Read(Adapter, E1000_REG_EERD));
            return false;
        }
        Mac[i * 2] = (uint8_t)Word;
        Mac[i * 2 + 1] = (uint8_t)(Word >> 8);
    }
    return E1000ValidMac(Mac);
}

static bool E1000LinkUp(NET_INTERFACE *Interface)
{
    E1000_ADAPTER *Adapter = Interface->Context;
    return (E1000Read(Adapter, E1000_REG_STATUS) & E1000_STATUS_LINK_UP) != 0;
}

static STATUS E1000Transmit(NET_INTERFACE *Interface, const void *Frame, uint32_t Length)
{
    if (!Frame || Length < 14 || Length > Interface->Mtu + 14)
        return STATUS_INVALID_PARAMETER;
    E1000_ADAPTER *Adapter = Interface->Context;
    KIRQL OldIrql;
    KeAcquireSpinLock(&Adapter->RingLock, &OldIrql);

    uint32_t Index = Adapter->TxIndex;
    volatile E1000_TX_DESCRIPTOR *Descriptor = &Adapter->TxRing[Index];
    if (!(Descriptor->Status & E1000_TX_STATUS_DD)) {
        KeReleaseSpinLock(&Adapter->RingLock, OldIrql);
        return STATUS_DEVICE_BUSY;
    }

    uint8_t *Buffer = Adapter->TxBuffers + Index * E1000_BUFFER_SIZE;
    memcpy(Buffer, Frame, Length);
    Descriptor->Address = (Adapter->TxDmaPfn << PAGE_SHIFT) +
                          E1000_RING_PAGES * PAGE_SIZE + (uint64_t)Index * E1000_BUFFER_SIZE;
    Descriptor->Length = (uint16_t)Length;
    Descriptor->ChecksumOffset = 0;
    Descriptor->ChecksumStart = 0;
    Descriptor->Special = 0;
    Descriptor->Command = E1000_TX_CMD_EOP | E1000_TX_CMD_IFCS | E1000_TX_CMD_RS;
    Descriptor->Status = 0;
    __asm__ volatile("" : : : "memory");
    Adapter->TxIndex = (Index + 1) % E1000_RING_COUNT;
    E1000Write(Adapter, E1000_REG_TDT, Adapter->TxIndex);
    KeReleaseSpinLock(&Adapter->RingLock, OldIrql);
    return STATUS_SUCCESS;
}

static STATUS E1000Receive(NET_INTERFACE *Interface, void *Frame, uint32_t Capacity, uint32_t *Length)
{
    if (!Frame || !Length)
        return STATUS_INVALID_PARAMETER;
    *Length = 0;
    E1000_ADAPTER *Adapter = Interface->Context;
    KIRQL OldIrql;
    KeAcquireSpinLock(&Adapter->RingLock, &OldIrql);

    uint32_t Index = Adapter->RxIndex;
    volatile E1000_RX_DESCRIPTOR *Descriptor = &Adapter->RxRing[Index];
    if (!(Descriptor->Status & E1000_RX_STATUS_DD)) {
        KeReleaseSpinLock(&Adapter->RingLock, OldIrql);
        return STATUS_NO_MORE_ENTRIES;
    }

    uint32_t PacketLength = Descriptor->Length;
    uint8_t Status = Descriptor->Status;
    uint8_t Errors = Descriptor->Errors;
    STATUS Result = STATUS_SUCCESS;
    if (!(Status & E1000_RX_STATUS_EOP) || Errors || PacketLength < 14 ||
        PacketLength > Interface->Mtu + 14 || PacketLength > E1000_BUFFER_SIZE) {
        Result = STATUS_DEVICE_DATA_ERROR;
    } else if (Capacity < PacketLength) {
        *Length = PacketLength;
        Result = STATUS_BUFFER_TOO_SMALL;
    } else {
        memcpy(Frame, Adapter->RxBuffers + Index * E1000_BUFFER_SIZE, PacketLength);
        *Length = PacketLength;
    }

    Descriptor->Status = 0;
    Descriptor->Errors = 0;
    Descriptor->Length = 0;
    __asm__ volatile("" : : : "memory");
    Adapter->RxIndex = (Index + 1) % E1000_RING_COUNT;
    E1000Write(Adapter, E1000_REG_RDT, Index);
    KeReleaseSpinLock(&Adapter->RingLock, OldIrql);
    return Result;
}

static void E1000ReleaseDma(E1000_ADAPTER *Adapter)
{
    if (Adapter->RxDmaPfn != MM_INVALID_PFN) {
        MmFreeContiguousPages(Adapter->RxDmaPfn, E1000_RING_PAGES + E1000_BUFFER_PAGES);
        Adapter->RxDmaPfn = MM_INVALID_PFN;
    }
    if (Adapter->TxDmaPfn != MM_INVALID_PFN) {
        MmFreeContiguousPages(Adapter->TxDmaPfn, E1000_RING_PAGES + E1000_BUFFER_PAGES);
        Adapter->TxDmaPfn = MM_INVALID_PFN;
    }
}

static bool E1000AllocateDma(E1000_ADAPTER *Adapter)
{
    Adapter->RxDmaPfn = Adapter->TxDmaPfn = MM_INVALID_PFN;
    if (!HZ_SUCCESS(MmAllocateContiguousPages(E1000_RING_PAGES + E1000_BUFFER_PAGES, E1000_DMA_MAX, 1,
                                               &Adapter->RxDmaPfn)))
        return false;
    if (!HZ_SUCCESS(MmAllocateContiguousPages(E1000_RING_PAGES + E1000_BUFFER_PAGES, E1000_DMA_MAX, 1,
                                               &Adapter->TxDmaPfn))) {
        E1000ReleaseDma(Adapter);
        return false;
    }
    uint8_t *RxMemory = MmPhysicalToVirtual(Adapter->RxDmaPfn << PAGE_SHIFT);
    uint8_t *TxMemory = MmPhysicalToVirtual(Adapter->TxDmaPfn << PAGE_SHIFT);
    Adapter->RxRing = (volatile E1000_RX_DESCRIPTOR *)RxMemory;
    Adapter->RxBuffers = RxMemory + E1000_RING_PAGES * PAGE_SIZE;
    Adapter->TxRing = (volatile E1000_TX_DESCRIPTOR *)TxMemory;
    Adapter->TxBuffers = TxMemory + E1000_RING_PAGES * PAGE_SIZE;
    memset(RxMemory, 0, (E1000_RING_PAGES + E1000_BUFFER_PAGES) * PAGE_SIZE);
    memset(TxMemory, 0, (E1000_RING_PAGES + E1000_BUFFER_PAGES) * PAGE_SIZE);
    return true;
}

static void E1000Initialize(E1000_ADAPTER *Adapter)
{
    E1000Write(Adapter, E1000_REG_IMC, 0xFFFFFFFFu);
    E1000Write(Adapter, E1000_REG_RCTL, 0);
    E1000Write(Adapter, E1000_REG_TCTL, 0);
    E1000Write(Adapter, E1000_REG_CTRL, E1000Read(Adapter, E1000_REG_CTRL) | E1000_CTRL_RST);
    if (!E1000WaitRegister(Adapter, E1000_REG_CTRL, E1000_CTRL_RST, 0, 100000)) {
        DbgPrintEx(DPFLTR_WARNING_LEVEL, "E1000: reset timed out\n");
        return;
    }

    if (!E1000ReadMac(Adapter, Adapter->Interface.MacAddress)) {
        DbgPrintEx(DPFLTR_WARNING_LEVEL, "E1000: could not read a valid MAC address from EEPROM\n");
        return;
    }
    if (!E1000AllocateDma(Adapter)) {
        DbgPrintEx(DPFLTR_WARNING_LEVEL, "E1000: could not allocate DMA rings below 4 GiB\n");
        return;
    }
    KeInitializeSpinLock(&Adapter->RingLock);

    for (uint32_t i = 0; i < E1000_RING_COUNT; i++) {
        Adapter->RxRing[i].Address = (Adapter->RxDmaPfn << PAGE_SHIFT) +
                                     E1000_RING_PAGES * PAGE_SIZE + (uint64_t)i * E1000_BUFFER_SIZE;
        Adapter->TxRing[i].Status = E1000_TX_STATUS_DD;
    }
    uint64_t RxRingPhysical = Adapter->RxDmaPfn << PAGE_SHIFT;
    uint64_t TxRingPhysical = Adapter->TxDmaPfn << PAGE_SHIFT;
    E1000Write(Adapter, E1000_REG_RDBAL, (uint32_t)RxRingPhysical);
    E1000Write(Adapter, E1000_REG_RDBAH, (uint32_t)(RxRingPhysical >> 32));
    E1000Write(Adapter, E1000_REG_RDLEN, E1000_RING_COUNT * sizeof(E1000_RX_DESCRIPTOR));
    E1000Write(Adapter, E1000_REG_RDH, 0);
    E1000Write(Adapter, E1000_REG_RDT, E1000_RING_COUNT - 1);
    E1000Write(Adapter, E1000_REG_TDBAL, (uint32_t)TxRingPhysical);
    E1000Write(Adapter, E1000_REG_TDBAH, (uint32_t)(TxRingPhysical >> 32));
    E1000Write(Adapter, E1000_REG_TDLEN, E1000_RING_COUNT * sizeof(E1000_TX_DESCRIPTOR));
    E1000Write(Adapter, E1000_REG_TDH, 0);
    E1000Write(Adapter, E1000_REG_TDT, 0);
    E1000Write(Adapter, E1000_REG_RAL,
               (uint32_t)Adapter->Interface.MacAddress[0] | ((uint32_t)Adapter->Interface.MacAddress[1] << 8) |
                   ((uint32_t)Adapter->Interface.MacAddress[2] << 16) |
                   ((uint32_t)Adapter->Interface.MacAddress[3] << 24));
    E1000Write(Adapter, E1000_REG_RAH, (uint32_t)Adapter->Interface.MacAddress[4] |
                                                  ((uint32_t)Adapter->Interface.MacAddress[5] << 8) | (1u << 31));
    E1000Write(Adapter, E1000_REG_TIPG, 0x0060200Au);
    E1000Write(Adapter, E1000_REG_CTRL, E1000Read(Adapter, E1000_REG_CTRL) | E1000_CTRL_SLU);
    E1000Write(Adapter, E1000_REG_TCTL, E1000_TCTL_EN | E1000_TCTL_PSP | (0xFu << 4) | (0x40u << 12));
    E1000Write(Adapter, E1000_REG_RCTL, E1000_RCTL_EN | E1000_RCTL_BAM | E1000_RCTL_SECRC);
}

static STATUS E1000DriverEntry(DRIVER_OBJECT *Driver)
{
    (void)Driver;
    uint32_t Found = 0;
    for (PCI_DEVICE *Pci = PciGetDeviceList(); Pci; Pci = Pci->Next) {
        bool Is82540Em = Pci->VendorId == E1000_VENDOR_ID && Pci->DeviceId == E1000_DEVICE_ID_82540EM;
        bool Is82574L = Pci->VendorId == E1000_VENDOR_ID && Pci->DeviceId == E1000_DEVICE_ID_82574L;
        bool Is82579Lm = Pci->VendorId == E1000_VENDOR_ID && Pci->DeviceId == E1000_DEVICE_ID_82579LM;
        if (Is82579Lm) {
            uint32_t Subsystem = PciReadConfig32(Pci, 0x2C);
            Is82579Lm = Pci->Revision == E1000_82579_REVISION &&
                        (uint16_t)Subsystem == E1000_82579_SUBVENDOR &&
                        (uint16_t)(Subsystem >> 16) == E1000_82579_SUBDEVICE;
        }
        if (!Is82540Em && !Is82574L && !Is82579Lm) {
            if (Pci->Class == 0x02 && Pci->VendorId == E1000_VENDOR_ID)
                DbgPrintEx(DPFLTR_WARNING_LEVEL, "E1000: unsupported network controller %04x:%04x at %02x:%02x.%u\n",
                           Pci->VendorId, Pci->DeviceId, Pci->Bus, Pci->Device, Pci->Function);
            continue;
        }
        const char *Model = Is82579Lm ? "Intel 82579LM Ethernet (experimental)" :
                            Is82574L ? "Intel 82574L Ethernet" : "Intel 82540EM Ethernet";
        if (!Pci->Bar[0] || Pci->BarIsIo[0] || Pci->BarSize[0] < 0x6000) {
            DbgPrintEx(DPFLTR_WARNING_LEVEL, "E1000: %02x:%02x.%u has an unusable BAR0\n", Pci->Bus, Pci->Device,
                       Pci->Function);
            PciSetDriverStatus(Pci, PciDriverStartFailed, Model);
            continue;
        }
        E1000_ADAPTER *Adapter = ExAllocatePoolZero(NonPagedPool, sizeof(*Adapter), E1000_TAG);
        if (!Adapter) {
            PciSetDriverStatus(Pci, PciDriverStartFailed, Model);
            return Found ? STATUS_SUCCESS : STATUS_INSUFFICIENT_RESOURCES;
        }
        Adapter->Pci = Pci;
        Adapter->RxDmaPfn = Adapter->TxDmaPfn = MM_INVALID_PFN;
        Adapter->Registers = MmMapIoSpace(Pci->Bar[0], 0x6000, MM_CACHE_UNCACHED);
        if (!Adapter->Registers) {
            ExFreePoolWithTag(Adapter, E1000_TAG);
            PciSetDriverStatus(Pci, PciDriverStartFailed, Model);
            continue;
        }
        PciEnableDevice(Pci, PCI_COMMAND_MEMORY | PCI_COMMAND_BUS_MASTER);
        Adapter->Interface.Context = Adapter;
        Adapter->Interface.Mtu = 1500;
        Adapter->Interface.SendFrame = E1000Transmit;
        Adapter->Interface.ReceiveFrame = E1000Receive;
        Adapter->Interface.QueryLink = E1000LinkUp;
        RtlFormatString(Adapter->Interface.Name, sizeof(Adapter->Interface.Name), "eth%u", Found);
        E1000Initialize(Adapter);
        if (Adapter->RxDmaPfn == MM_INVALID_PFN || Adapter->TxDmaPfn == MM_INVALID_PFN) {
            E1000ReleaseDma(Adapter);
            ExFreePoolWithTag(Adapter, E1000_TAG);
            PciSetDriverStatus(Pci, PciDriverStartFailed, Model);
            continue;
        }
        STATUS Status = NetRegisterInterface(&Adapter->Interface);
        if (!HZ_SUCCESS(Status)) {
            DbgPrintEx(DPFLTR_WARNING_LEVEL, "E1000: could not register %s (0x%08X)\n",
                       Adapter->Interface.Name, (uint32_t)Status);
            E1000ReleaseDma(Adapter);
            ExFreePoolWithTag(Adapter, E1000_TAG);
            PciSetDriverStatus(Pci, PciDriverStartFailed, Model);
            continue;
        }
        PciSetDriverStatus(Pci, PciDriverStarted, Model);
        DbgPrint("E1000: %02x:%02x.%u 8086:%04X (%s), %s link\n", Pci->Bus, Pci->Device, Pci->Function,
                 Pci->DeviceId, Model,
                 E1000LinkUp(&Adapter->Interface) ? "up" : "down");
        Found++;
    }
    return Found ? STATUS_SUCCESS : STATUS_NO_SUCH_DEVICE;
}

IO_DECLARE_BUILTIN_DRIVER(E1000BuiltinDriver, "\\Driver\\E1000", E1000DriverEntry, IO_ORDER_DEVICE);
