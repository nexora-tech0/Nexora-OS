/* Nexora OS - basic polled Realtek RTL8139 PCI Ethernet driver. */
#include "ex.h"
#include "hal.h"
#include "io.h"
#include "ke.h"
#include "mm.h"
#include "netif.h"
#include "pci.h"
#include "rtl.h"

#define RTL8139_TAG HZ_POOL_TAG('R', '8', '1', '3')
#define RTL8139_VENDOR 0x10ECu
#define RTL8139_DEVICE 0x8139u
#define RTL8139_RING_BYTES 32768u
#define RTL8139_RING_PAGES 9u
#define RTL8139_TX_COUNT 4u
#define RTL8139_TX_BYTES 2048u
#define RTL8139_TX_PAGES 2u
#define RTL8139_RX_DMA_LIMIT 0x100000000ull
#define RTL8139_CR 0x37u
#define RTL8139_CAPR 0x38u
#define RTL8139_ISR 0x3Eu
#define RTL8139_TCR 0x40u
#define RTL8139_RCR 0x44u
#define RTL8139_CFG9346 0x50u
#define RTL8139_CONFIG1 0x52u
#define RTL8139_MSR 0x58u
#define RTL8139_RBSTART 0x30u
#define RTL8139_CR_BUFE 0x01u
#define RTL8139_CR_RESET 0x10u
#define RTL8139_CR_RX_ENABLE 0x08u
#define RTL8139_CR_TX_ENABLE 0x04u
#define RTL8139_TSD_BASE 0x10u
#define RTL8139_TSAD_BASE 0x20u
#define RTL8139_MSR_LINK 0x04u
#define RTL8139_TSD_OWN (1u << 13)
#define RTL8139_TSD_OK (1u << 15)

typedef struct _RTL8139_ADAPTER {
    PCI_DEVICE *Pci;
    uint16_t IoBase;
    PFN_NUMBER RxPfn, TxPfn;
    uint8_t *RxRing, *TxBuffers;
    uint32_t RxOffset, TxIndex;
    KSPIN_LOCK Lock;
    NET_INTERFACE Interface;
} RTL8139_ADAPTER;

static bool Rtl8139Link(NET_INTERFACE *Interface)
{
    RTL8139_ADAPTER *A = Interface->Context;
    /* LINKB is active-low: zero means the PHY has a valid link. */
    return (HalReadPort8(A->IoBase + RTL8139_MSR) & RTL8139_MSR_LINK) == 0;
}

static STATUS Rtl8139Send(NET_INTERFACE *Interface, const void *Frame, uint32_t Length)
{
    if (!Frame || Length < 14 || Length > Interface->Mtu + 14)
        return STATUS_INVALID_PARAMETER;
    RTL8139_ADAPTER *A = Interface->Context;
    KIRQL Old;
    KeAcquireSpinLock(&A->Lock, &Old);
    uint32_t Index = A->TxIndex;
    uint32_t Tsd = A->IoBase + RTL8139_TSD_BASE + Index * 4;
    uint32_t TxStatus = HalReadPort32((uint16_t)Tsd);
    /* TxHostOwns means the descriptor is available for the host to submit. */
    if (!(TxStatus & RTL8139_TSD_OWN)) {
        static uint32_t BusyLogCount;
        if (BusyLogCount++ < 4)
            DbgPrintEx(DPFLTR_WARNING_LEVEL, "RTL8139: TX descriptor %u busy, TSD=%08x ISR=%04x\n",
                       Index, TxStatus, HalReadPort16(A->IoBase + RTL8139_ISR));
        KeReleaseSpinLock(&A->Lock, Old);
        return STATUS_DEVICE_BUSY;
    }
    uint8_t *Buffer = A->TxBuffers + Index * RTL8139_TX_BYTES;
    memcpy(Buffer, Frame, Length);
    HalWritePort32((uint16_t)(A->IoBase + RTL8139_TSAD_BASE + Index * 4),
                   (uint32_t)((A->TxPfn << PAGE_SHIFT) + (uint64_t)Index * RTL8139_TX_BYTES));
    __asm__ volatile("" : : : "memory");
    HalWritePort32((uint16_t)Tsd, Length);
    static uint32_t SendLogCount;
    if (SendLogCount++ < 4)
        DbgPrint("RTL8139: queued TX descriptor %u length %u, prior TSD=%08x now=%08x\n",
                 Index, Length, TxStatus, HalReadPort32((uint16_t)Tsd));
    A->TxIndex = (Index + 1) % RTL8139_TX_COUNT;
    KeReleaseSpinLock(&A->Lock, Old);
    return STATUS_SUCCESS;
}

static STATUS Rtl8139Receive(NET_INTERFACE *Interface, void *Frame, uint32_t Capacity, uint32_t *Length)
{
    if (!Frame || !Length) return STATUS_INVALID_PARAMETER;
    *Length = 0;
    RTL8139_ADAPTER *A = Interface->Context;
    KIRQL Old;
    KeAcquireSpinLock(&A->Lock, &Old);
    uint8_t Command = HalReadPort8(A->IoBase + RTL8139_CR);
    if (Command & RTL8139_CR_BUFE) {
        static bool EmptyLogged;
        if (!EmptyLogged) {
            EmptyLogged = true;
            DbgPrint("RTL8139: RX ring empty, CR=%02x RCR=%08x ISR=%04x\n", Command,
                     HalReadPort32(A->IoBase + RTL8139_RCR), HalReadPort16(A->IoBase + RTL8139_ISR));
        }
        KeReleaseSpinLock(&A->Lock, Old);
        return STATUS_NO_MORE_ENTRIES;
    }
    uint8_t *Ring = A->RxRing;
    uint32_t Offset = A->RxOffset;
    uint16_t RxStatus = (uint16_t)(Ring[Offset] | ((uint16_t)Ring[(Offset + 1) % RTL8139_RING_BYTES] << 8));
    uint16_t PacketLength = (uint16_t)(Ring[(Offset + 2) % RTL8139_RING_BYTES] |
                              ((uint16_t)Ring[(Offset + 3) % RTL8139_RING_BYTES] << 8));
    static uint32_t ReceiveLogCount;
    if (ReceiveLogCount++ < 8)
        DbgPrint("RTL8139: RX entry offset=%u status=%04x length=%u CR=%02x\n",
                 Offset, RxStatus, PacketLength, Command);
    if (!(RxStatus & 1u) || PacketLength < 18 || PacketLength > Interface->Mtu + 18) {
        A->RxOffset = 0;
        HalWritePort16(A->IoBase + RTL8139_CAPR, 0xFFF0u);
        KeReleaseSpinLock(&A->Lock, Old);
        return STATUS_DEVICE_DATA_ERROR;
    }
    uint32_t FrameLength = PacketLength - 4;
    if (Capacity < FrameLength) {
        *Length = FrameLength;
        KeReleaseSpinLock(&A->Lock, Old);
        return STATUS_BUFFER_TOO_SMALL;
    }
    uint8_t *Out = Frame;
    for (uint32_t I = 0; I < FrameLength; I++)
        Out[I] = Ring[(Offset + 4 + I) % RTL8139_RING_BYTES];
    *Length = FrameLength;
    A->RxOffset = (Offset + 4 + PacketLength + 3) & ~3u;
    A->RxOffset %= RTL8139_RING_BYTES;
    HalWritePort16(A->IoBase + RTL8139_CAPR, (uint16_t)(A->RxOffset - 16));
    KeReleaseSpinLock(&A->Lock, Old);
    return STATUS_SUCCESS;
}

static bool Rtl8139Start(RTL8139_ADAPTER *A)
{
    uint16_t Base = A->IoBase;
    HalWritePort8(Base + RTL8139_CR, RTL8139_CR_RESET);
    uint32_t Wait;
    for (Wait = 0; Wait < 100000 && (HalReadPort8(Base + RTL8139_CR) & RTL8139_CR_RESET); Wait++)
        KeStallExecutionProcessor(10);
    if (Wait == 100000) return false;
    HalWritePort8(Base + RTL8139_CFG9346, 0xC0);
    HalWritePort8(Base + RTL8139_CONFIG1, 0);
    HalWritePort8(Base + RTL8139_CFG9346, 0);

    for (uint32_t I = 0; I < 6; I++) A->Interface.MacAddress[I] = HalReadPort8(Base + I);
    if (!A->Interface.MacAddress[0] || (A->Interface.MacAddress[0] & 1)) return false;
    if (!HZ_SUCCESS(MmAllocateContiguousPages(RTL8139_RING_PAGES, RTL8139_RX_DMA_LIMIT, 1, &A->RxPfn)) ||
        !HZ_SUCCESS(MmAllocateContiguousPages(RTL8139_TX_PAGES, RTL8139_RX_DMA_LIMIT, 1, &A->TxPfn)) ||
        !(A->RxRing = MmPhysicalToVirtual(A->RxPfn << PAGE_SHIFT)) ||
        !(A->TxBuffers = MmPhysicalToVirtual(A->TxPfn << PAGE_SHIFT)))
        return false;
    memset(A->RxRing, 0, RTL8139_RING_PAGES * PAGE_SIZE);
    memset(A->TxBuffers, 0, RTL8139_TX_PAGES * PAGE_SIZE);
    A->RxOffset = A->TxIndex = 0;
    KeInitializeSpinLock(&A->Lock);
    HalWritePort32(Base + RTL8139_RBSTART, (uint32_t)(A->RxPfn << PAGE_SHIFT));
    HalWritePort16(Base + RTL8139_CAPR, 0xFFF0u);
    HalWritePort16(Base + RTL8139_ISR, 0xFFFFu);
    HalWritePort16(Base + 0x3Cu, 0); /* polled by the shared network worker */
    HalWritePort32(Base + RTL8139_TCR, 0);
    HalWritePort32(Base + RTL8139_RCR, (1u << 12) | (1u << 7) | (1u << 3) | 1u); /* 32 KiB, wrap, broadcast, all physical */
    HalWritePort8(Base + RTL8139_CR, RTL8139_CR_RX_ENABLE | RTL8139_CR_TX_ENABLE);
    return true;
}

static STATUS Rtl8139DriverEntry(DRIVER_OBJECT *Driver)
{
    (void)Driver;
    uint32_t Found = 0;
    for (PCI_DEVICE *Pci = PciGetDeviceList(); Pci; Pci = Pci->Next) {
        if (Pci->VendorId != RTL8139_VENDOR || Pci->DeviceId != RTL8139_DEVICE) continue;
        const char *Model = "Realtek RTL8139 Ethernet";
        if (!Pci->Bar[0] || !Pci->BarIsIo[0] || Pci->BarSize[0] < 0x100) {
            PciSetDriverStatus(Pci, PciDriverStartFailed, Model); continue;
        }
        RTL8139_ADAPTER *A = ExAllocatePoolZero(NonPagedPool, sizeof(*A), RTL8139_TAG);
        if (!A) { PciSetDriverStatus(Pci, PciDriverStartFailed, Model); continue; }
        A->Pci = Pci; A->IoBase = (uint16_t)Pci->Bar[0]; A->RxPfn = A->TxPfn = MM_INVALID_PFN;
        A->Interface.Context = A; A->Interface.Mtu = 1500;
        A->Interface.SendFrame = Rtl8139Send; A->Interface.ReceiveFrame = Rtl8139Receive;
        A->Interface.QueryLink = Rtl8139Link;
        RtlFormatString(A->Interface.Name, sizeof(A->Interface.Name), "eth%u", Found);
        PciEnableDevice(Pci, PCI_COMMAND_IO | PCI_COMMAND_BUS_MASTER);
        if (!Rtl8139Start(A)) {
            if (A->RxPfn != MM_INVALID_PFN) MmFreeContiguousPages(A->RxPfn, RTL8139_RING_PAGES);
            if (A->TxPfn != MM_INVALID_PFN) MmFreeContiguousPages(A->TxPfn, RTL8139_TX_PAGES);
            ExFreePoolWithTag(A, RTL8139_TAG); PciSetDriverStatus(Pci, PciDriverStartFailed, Model); continue;
        }
        STATUS Status = NetRegisterInterface(&A->Interface);
        if (!HZ_SUCCESS(Status)) {
            MmFreeContiguousPages(A->RxPfn, RTL8139_RING_PAGES); MmFreeContiguousPages(A->TxPfn, RTL8139_TX_PAGES);
            ExFreePoolWithTag(A, RTL8139_TAG); PciSetDriverStatus(Pci, PciDriverStartFailed, Model); continue;
        }
        PciSetDriverStatus(Pci, PciDriverStarted, Model);
        DbgPrint("RTL8139: %s, %02x:%02x.%u, %04x:%04x, MAC %02x:%02x:%02x:%02x:%02x:%02x, %s link\n",
                 Model, Pci->Bus, Pci->Device, Pci->Function, Pci->VendorId, Pci->DeviceId,
                 A->Interface.MacAddress[0], A->Interface.MacAddress[1], A->Interface.MacAddress[2],
                 A->Interface.MacAddress[3], A->Interface.MacAddress[4], A->Interface.MacAddress[5],
                 Rtl8139Link(&A->Interface) ? "up" : "down");
        Found++;
    }
    return Found ? STATUS_SUCCESS : STATUS_NO_SUCH_DEVICE;
}

IO_DECLARE_BUILTIN_DRIVER(Rtl8139BuiltinDriver, "\\Driver\\RTL8139", Rtl8139DriverEntry, IO_ORDER_DEVICE);
