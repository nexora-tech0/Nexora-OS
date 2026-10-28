/*
 * Nexora OS - AMD PCnet PCI Ethernet driver (Am79C970/Am79C973 style).
 * 32-bit software style, bounded polled descriptor rings, and one adapter family ID at a time.
 */
#include "ex.h"
#include "hal.h"
#include "io.h"
#include "ke.h"
#include "mm.h"
#include "netif.h"
#include "pci.h"
#include "rtl.h"

#define PCNET_TAG HZ_POOL_TAG('P', 'C', 'N', 'T')
#define PCNET_VENDOR_AMD 0x1022u
#define PCNET_DEVICE 0x2000u
#define PCNET_RING_COUNT 8u
#define PCNET_BUFFER_SIZE 2048u
#define PCNET_DMA_PAGES 9u /* one init/ring page plus eight pages for 16 packet buffers */
#define PCNET_DMA_LIMIT 0x100000000ull
#define PCNET_RDP 0x10u
#define PCNET_RAP 0x14u
#define PCNET_WIO_RAP 0x12u
#define PCNET_WIO_BDP 0x16u
#define PCNET_WIO_RESET 0x14u
#define PCNET_RESET 0x18u
#define PCNET_BDP 0x1Cu
#define PCNET_CSR0 0u
#define PCNET_CSR1 1u
#define PCNET_CSR2 2u
#define PCNET_CSR3 3u
#define PCNET_CSR0_INIT 0x0001u
#define PCNET_CSR0_START 0x0002u
#define PCNET_CSR0_STOP 0x0004u
#define PCNET_CSR0_TX_DEMAND 0x0008u
#define PCNET_CSR0_IDON 0x0100u
#define PCNET_CSR0_INTERRUPTS 0x0040u
#define PCNET_BCR20 20u
#define PCNET_SWSTYLE_32 2u
#define PCNET_DESC_OWN 0x8000u
#define PCNET_DESC_STP 0x0200u
#define PCNET_DESC_ENP 0x0100u
#define PCNET_DESC_ERR 0x4000u

typedef struct HZ_PACKED _PCNET_DESCRIPTOR {
    uint32_t Address;
    uint16_t BufferLength;
    uint16_t Status;
    uint32_t MessageLength;
    uint32_t Reserved;
} PCNET_DESCRIPTOR;

typedef struct HZ_PACKED _PCNET_INIT_BLOCK {
    uint16_t Mode;
    uint16_t TxRxRingLength;
    uint8_t PhysicalAddress[6];
    uint16_t Reserved;
    uint32_t MulticastFilter[2];
    uint32_t RxRing;
    uint32_t TxRing;
} PCNET_INIT_BLOCK;

_Static_assert(sizeof(PCNET_DESCRIPTOR) == 16, "PCnet descriptor size");
_Static_assert(sizeof(PCNET_INIT_BLOCK) == 28, "PCnet 32-bit init block size");

typedef struct _PCNET_ADAPTER {
    PCI_DEVICE *Pci;
    uint16_t IoBase;
    PFN_NUMBER DmaPfn;
    uint8_t *Dma;
    PCNET_DESCRIPTOR *RxRing;
    PCNET_DESCRIPTOR *TxRing;
    uint32_t RxIndex;
    uint32_t TxIndex;
    KSPIN_LOCK Lock;
    NET_INTERFACE Interface;
} PCNET_ADAPTER;

static uint16_t PcnetReadCsr(PCNET_ADAPTER *Adapter, uint32_t Index)
{
    HalWritePort32((uint16_t)(Adapter->IoBase + PCNET_RAP), Index);
    return (uint16_t)HalReadPort32((uint16_t)(Adapter->IoBase + PCNET_RDP));
}

static void PcnetWriteCsr(PCNET_ADAPTER *Adapter, uint32_t Index, uint16_t Value)
{
    HalWritePort32((uint16_t)(Adapter->IoBase + PCNET_RAP), Index);
    HalWritePort32((uint16_t)(Adapter->IoBase + PCNET_RDP), Value);
}

static void PcnetSelect32BitStyle(PCNET_ADAPTER *Adapter)
{
    /* After reset PCnet is in 16-bit/WIO mode. BCR20 must be programmed using
       16-bit RAP/BDP cycles before the 32-bit register layout can be used. */
    HalWritePort16((uint16_t)(Adapter->IoBase + PCNET_WIO_RAP), PCNET_BCR20);
    HalWritePort16((uint16_t)(Adapter->IoBase + PCNET_WIO_BDP), PCNET_SWSTYLE_32);
}

static bool PcnetLink(NET_INTERFACE *Interface)
{
    PCNET_ADAPTER *Adapter = Interface->Context;
    /* PCnet exposes PHY link state through model-specific MII registers. Until that path is
       implemented, report the controller's started state; DHCP still times out on disconnected
       media and the UI must not mistake this as a physical link LED implementation. */
    return (PcnetReadCsr(Adapter, PCNET_CSR0) & PCNET_CSR0_STOP) == 0;
}

static STATUS PcnetSend(NET_INTERFACE *Interface, const void *Frame, uint32_t Length)
{
    if (!Frame || Length < 14 || Length > Interface->Mtu + 14 || Length > PCNET_BUFFER_SIZE)
        return STATUS_INVALID_PARAMETER;
    PCNET_ADAPTER *Adapter = Interface->Context;
    KIRQL OldIrql;
    KeAcquireSpinLock(&Adapter->Lock, &OldIrql);
    uint32_t Index = Adapter->TxIndex;
    PCNET_DESCRIPTOR *Descriptor = &Adapter->TxRing[Index];
    if (Descriptor->Status & PCNET_DESC_OWN) {
        KeReleaseSpinLock(&Adapter->Lock, OldIrql);
        return STATUS_DEVICE_BUSY;
    }
    uint8_t *Buffer = Adapter->Dma + PAGE_SIZE +
                      (PCNET_RING_COUNT + Index) * PCNET_BUFFER_SIZE;
    memcpy(Buffer, Frame, Length);
    Descriptor->Address = (uint32_t)((Adapter->DmaPfn << PAGE_SHIFT) + PAGE_SIZE +
                                     (uint64_t)(PCNET_RING_COUNT + Index) * PCNET_BUFFER_SIZE);
    Descriptor->BufferLength = (uint16_t)(0u - Length);
    Descriptor->MessageLength = 0;
    Descriptor->Reserved = 0;
    __asm__ volatile("" : : : "memory");
    Descriptor->Status = PCNET_DESC_OWN | PCNET_DESC_STP | PCNET_DESC_ENP;
    Adapter->TxIndex = (Index + 1) % PCNET_RING_COUNT;
    PcnetWriteCsr(Adapter, PCNET_CSR0, PCNET_CSR0_TX_DEMAND);
    KeReleaseSpinLock(&Adapter->Lock, OldIrql);
    return STATUS_SUCCESS;
}

static STATUS PcnetReceive(NET_INTERFACE *Interface, void *Frame, uint32_t Capacity, uint32_t *Length)
{
    if (!Frame || !Length)
        return STATUS_INVALID_PARAMETER;
    *Length = 0;
    PCNET_ADAPTER *Adapter = Interface->Context;
    KIRQL OldIrql;
    KeAcquireSpinLock(&Adapter->Lock, &OldIrql);
    uint32_t Index = Adapter->RxIndex;
    PCNET_DESCRIPTOR *Descriptor = &Adapter->RxRing[Index];
    if (Descriptor->Status & PCNET_DESC_OWN) {
        KeReleaseSpinLock(&Adapter->Lock, OldIrql);
        return STATUS_NO_MORE_ENTRIES;
    }
    uint16_t Status = Descriptor->Status;
    uint32_t PacketLength = Descriptor->MessageLength & 0x0FFFu;
    STATUS Result = STATUS_SUCCESS;
    if ((Status & PCNET_DESC_ERR) || (Status & (PCNET_DESC_STP | PCNET_DESC_ENP)) !=
        (PCNET_DESC_STP | PCNET_DESC_ENP) || PacketLength < 18 || PacketLength > PCNET_BUFFER_SIZE) {
        Result = STATUS_DEVICE_DATA_ERROR;
    } else {
        PacketLength -= 4; /* the controller includes the Ethernet CRC in the message length */
        if (Capacity < PacketLength) {
            *Length = PacketLength;
            Result = STATUS_BUFFER_TOO_SMALL;
        } else {
            uint8_t *Buffer = Adapter->Dma + PAGE_SIZE + Index * PCNET_BUFFER_SIZE;
            memcpy(Frame, Buffer, PacketLength);
            *Length = PacketLength;
        }
    }
    Descriptor->MessageLength = 0;
    __asm__ volatile("" : : : "memory");
    Descriptor->Status = PCNET_DESC_OWN;
    Adapter->RxIndex = (Index + 1) % PCNET_RING_COUNT;
    KeReleaseSpinLock(&Adapter->Lock, OldIrql);
    return Result;
}

static bool PcnetStart(PCNET_ADAPTER *Adapter)
{
    uint16_t Base = Adapter->IoBase;
    (void)HalReadPort16((uint16_t)(Base + PCNET_WIO_RESET));
    KeStallExecutionProcessor(1000);

    /* The address PROM is byte-wide in WIO mode; read it before changing styles. */
    for (uint32_t Index = 0; Index < 6; ++Index)
        Adapter->Interface.MacAddress[Index] = HalReadPort8((uint16_t)(Base + Index));
    DbgPrint("PCnet: PROM MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
             Adapter->Interface.MacAddress[0], Adapter->Interface.MacAddress[1],
             Adapter->Interface.MacAddress[2], Adapter->Interface.MacAddress[3],
             Adapter->Interface.MacAddress[4], Adapter->Interface.MacAddress[5]);
    if (!Adapter->Interface.MacAddress[0] || (Adapter->Interface.MacAddress[0] & 1u)) {
        DbgPrintEx(DPFLTR_WARNING_LEVEL, "PCnet: invalid PROM MAC address\n");
        return false;
    }

    PcnetSelect32BitStyle(Adapter);
    PcnetWriteCsr(Adapter, PCNET_CSR0, PCNET_CSR0_STOP);

    if (!HZ_SUCCESS(MmAllocateContiguousPages(PCNET_DMA_PAGES, PCNET_DMA_LIMIT, 1, &Adapter->DmaPfn))) {
        DbgPrintEx(DPFLTR_WARNING_LEVEL, "PCnet: DMA allocation failed\n");
        return false;
    }
    Adapter->Dma = MmPhysicalToVirtual(Adapter->DmaPfn << PAGE_SHIFT);
    if (!Adapter->Dma) {
        DbgPrintEx(DPFLTR_WARNING_LEVEL, "PCnet: DMA mapping failed\n");
        return false;
    }
    memset(Adapter->Dma, 0, PCNET_DMA_PAGES * PAGE_SIZE);
    Adapter->RxRing = (PCNET_DESCRIPTOR *)Adapter->Dma;
    Adapter->TxRing = Adapter->RxRing + PCNET_RING_COUNT;
    uint64_t Physical = Adapter->DmaPfn << PAGE_SHIFT;
    for (uint32_t Index = 0; Index < PCNET_RING_COUNT; ++Index) {
        Adapter->RxRing[Index].Address = (uint32_t)(Physical + PAGE_SIZE + (uint64_t)Index * PCNET_BUFFER_SIZE);
        Adapter->RxRing[Index].BufferLength = (uint16_t)(0u - PCNET_BUFFER_SIZE);
        Adapter->RxRing[Index].Status = PCNET_DESC_OWN;
        Adapter->TxRing[Index].Address = (uint32_t)(Physical + PAGE_SIZE +
                                                   (uint64_t)(PCNET_RING_COUNT + Index) * PCNET_BUFFER_SIZE);
    }

    PCNET_INIT_BLOCK *Init = (PCNET_INIT_BLOCK *)(Adapter->Dma + 256);
    Init->Mode = 0;
    /* The low byte is RLEN and the high byte TLEN; each exponent is encoded
       in the upper nibble of its byte (3 means eight descriptors). */
    Init->TxRxRingLength = 0x3330u;
    memcpy(Init->PhysicalAddress, Adapter->Interface.MacAddress, sizeof(Init->PhysicalAddress));
    Init->RxRing = (uint32_t)Physical;
    Init->TxRing = (uint32_t)(Physical + PCNET_RING_COUNT * sizeof(PCNET_DESCRIPTOR));

    KeInitializeSpinLock(&Adapter->Lock);
    PcnetWriteCsr(Adapter, PCNET_CSR3, 0x0000u); /* polled mode: do not mask status reporting */
    PcnetWriteCsr(Adapter, PCNET_CSR1, (uint16_t)((Physical + 256) & 0xFFFFu));
    PcnetWriteCsr(Adapter, PCNET_CSR2, (uint16_t)((Physical + 256) >> 16));
    PcnetWriteCsr(Adapter, PCNET_CSR0, PCNET_CSR0_INIT);
    uint32_t Elapsed;
    for (Elapsed = 0; Elapsed < 500000; Elapsed += 10) {
        if (PcnetReadCsr(Adapter, PCNET_CSR0) & PCNET_CSR0_IDON)
            break;
        HalStallExecution(10);
    }
    if (Elapsed == 500000) {
        DbgPrintEx(DPFLTR_WARNING_LEVEL, "PCnet: initialization timed out (CSR0=%04x)\n",
                   PcnetReadCsr(Adapter, PCNET_CSR0));
        return false;
    }
    PcnetWriteCsr(Adapter, PCNET_CSR0, PCNET_CSR0_IDON | PCNET_CSR0_START);
    return true;
}

static STATUS PcnetDriverEntry(DRIVER_OBJECT *Driver)
{
    (void)Driver;
    DbgPrint("PCnet: scanning PCI network devices\n");
    uint32_t Started = 0;
    for (PCI_DEVICE *Pci = PciGetDeviceList(); Pci; Pci = Pci->Next) {
        if (Pci->Class == 0x02)
            DbgPrint("PCnet: inspecting network-class PCI %04x:%04x at %02x:%02x.%u\n", Pci->VendorId,
                     Pci->DeviceId, Pci->Bus, Pci->Device, Pci->Function);
        if (Pci->VendorId != PCNET_VENDOR_AMD || Pci->DeviceId != PCNET_DEVICE)
            continue;
        const char *Model = "AMD PCnet PCI Ethernet";
        DbgPrint("PCnet: BAR0=%llx io=%u size=%llx\n",
                 (unsigned long long)Pci->Bar[0], Pci->BarIsIo[0],
                 (unsigned long long)Pci->BarSize[0]);
        if (!Pci->Bar[0] || !Pci->BarIsIo[0] || Pci->BarSize[0] < 0x20) {
            DbgPrintEx(DPFLTR_WARNING_LEVEL, "PCnet: unsupported I/O BAR layout\n");
            PciSetDriverStatus(Pci, PciDriverStartFailed, Model);
            continue;
        }
        PCNET_ADAPTER *Adapter = ExAllocatePoolZero(NonPagedPool, sizeof(*Adapter), PCNET_TAG);
        if (!Adapter) {
            PciSetDriverStatus(Pci, PciDriverStartFailed, Model);
            continue;
        }
        Adapter->Pci = Pci;
        Adapter->IoBase = (uint16_t)Pci->Bar[0];
        Adapter->DmaPfn = MM_INVALID_PFN;
        Adapter->Interface.Context = Adapter;
        Adapter->Interface.Mtu = 1500;
        Adapter->Interface.SendFrame = PcnetSend;
        Adapter->Interface.ReceiveFrame = PcnetReceive;
        Adapter->Interface.QueryLink = PcnetLink;
        RtlFormatString(Adapter->Interface.Name, sizeof(Adapter->Interface.Name), "eth%u", Started);
        PciEnableDevice(Pci, PCI_COMMAND_IO | PCI_COMMAND_BUS_MASTER);
        if (!PcnetStart(Adapter)) {
            DbgPrintEx(DPFLTR_WARNING_LEVEL, "PCnet: controller start failed\n");
            if (Adapter->DmaPfn != MM_INVALID_PFN)
                MmFreeContiguousPages(Adapter->DmaPfn, PCNET_DMA_PAGES);
            ExFreePoolWithTag(Adapter, PCNET_TAG);
            PciSetDriverStatus(Pci, PciDriverStartFailed, Model);
            continue;
        }
        if (!HZ_SUCCESS(NetRegisterInterface(&Adapter->Interface))) {
            DbgPrintEx(DPFLTR_WARNING_LEVEL, "PCnet: network interface registration failed\n");
            if (Adapter->DmaPfn != MM_INVALID_PFN)
                MmFreeContiguousPages(Adapter->DmaPfn, PCNET_DMA_PAGES);
            ExFreePoolWithTag(Adapter, PCNET_TAG);
            PciSetDriverStatus(Pci, PciDriverStartFailed, Model);
            continue;
        }
        PciSetDriverStatus(Pci, PciDriverStarted, Model);
        DbgPrint("PCnet: %02x:%02x.%u %04x:%04x MAC %02x:%02x:%02x:%02x:%02x:%02x, DMA rings started\n",
                 Pci->Bus, Pci->Device, Pci->Function, Pci->VendorId, Pci->DeviceId,
                 Adapter->Interface.MacAddress[0], Adapter->Interface.MacAddress[1],
                 Adapter->Interface.MacAddress[2], Adapter->Interface.MacAddress[3],
                 Adapter->Interface.MacAddress[4], Adapter->Interface.MacAddress[5]);
        Started++;
    }
    return Started ? STATUS_SUCCESS : STATUS_NO_SUCH_DEVICE;
}

IO_DECLARE_BUILTIN_DRIVER(PcnetBuiltinDriver, "\\Driver\\Pcnet", PcnetDriverEntry, IO_ORDER_DEVICE);
