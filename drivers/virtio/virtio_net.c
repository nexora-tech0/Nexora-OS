/*
 * Nexora OS - drivers/virtio/virtio_net.c
 * Legacy/transitional PCI VirtIO network device (1AF4:1000). This polled driver negotiates only
 * the device MAC feature, carries unoffloaded Ethernet frames through RX/TX virtqueues, and hooks
 * into Nexora's ordinary NET_INTERFACE/DHCP stack. Modern VirtIO PCI is not supported here.
 */
#include "ex.h"
#include "hal.h"
#include "io.h"
#include "ke.h"
#include "mm.h"
#include "netif.h"
#include "pci.h"
#include "rtl.h"

#define VNET_TAG HZ_POOL_TAG('V', 'N', 'e', 't')
#define VNET_VENDOR 0x1AF4u
#define VNET_DEVICE 0x1000u
#define VNET_BUFFER_COUNT 16u
#define VNET_BUFFER_SIZE 2048u
#define VNET_BUFFER_PAGES (VNET_BUFFER_COUNT * VNET_BUFFER_SIZE / PAGE_SIZE)
#define VNET_QUEUE_PAGES 8u
#define VNET_DMA_LIMIT 0x100000000ull
#define VNET_TIMEOUT_US 5000000u
#define VNET_FEATURE_MAC (1u << 5)
#define VNET_STATUS_ACKNOWLEDGE 1u
#define VNET_STATUS_DRIVER 2u
#define VNET_STATUS_DRIVER_OK 4u
#define VNET_STATUS_FAILED 128u
#define VNET_DESC_NEXT 1u
#define VNET_DESC_DEVICE_WRITE 2u

typedef struct HZ_PACKED _VNET_DESCRIPTOR {
    uint64_t Address;
    uint32_t Length;
    uint16_t Flags;
    uint16_t Next;
} VNET_DESCRIPTOR;

typedef struct HZ_PACKED _VNET_USED_ELEMENT {
    uint32_t Id;
    uint32_t Length;
} VNET_USED_ELEMENT;

typedef struct HZ_PACKED _VNET_HEADER {
    uint8_t Flags;
    uint8_t GsoType;
    uint16_t HeaderLength;
    uint16_t GsoSize;
    uint16_t ChecksumStart;
    uint16_t ChecksumOffset;
} VNET_HEADER;

typedef struct _VNET_QUEUE {
    PFN_NUMBER RingPfn;
    uint8_t *RingMemory;
    volatile VNET_DESCRIPTOR *Descriptors;
    volatile uint16_t *AvailableIndex;
    volatile uint16_t *AvailableRing;
    volatile uint16_t *UsedIndex;
    volatile VNET_USED_ELEMENT *UsedRing;
    uint32_t UsedOffset;
    uint16_t Size;
    uint16_t AvailableTail;
    uint16_t UsedHead;
} VNET_QUEUE;

typedef struct _VNET_ADAPTER {
    PCI_DEVICE *Pci;
    uint16_t IoBase;
    PFN_NUMBER RxDataPfn;
    PFN_NUMBER TxDataPfn;
    uint8_t *RxData;
    uint8_t *TxData;
    VNET_QUEUE Rx;
    VNET_QUEUE Tx;
    KMUTEX RxLock;
    KMUTEX TxLock;
    NET_INTERFACE Interface;
} VNET_ADAPTER;

static bool VnetInitializeQueue(VNET_ADAPTER *Adapter, VNET_QUEUE *Queue, uint16_t QueueIndex)
{
    uint16_t Base = Adapter->IoBase;
    HalWritePort16(Base + 0x0Eu, QueueIndex);
    Queue->Size = HalReadPort16(Base + 0x0Cu);
    if (Queue->Size < VNET_BUFFER_COUNT || Queue->Size > 1024u || (Queue->Size & (Queue->Size - 1u)))
        return false;
    if (!HZ_SUCCESS(MmAllocateContiguousPages(VNET_QUEUE_PAGES, VNET_DMA_LIMIT, 1, &Queue->RingPfn)))
        return false;
    Queue->RingMemory = MmPhysicalToVirtual(Queue->RingPfn << PAGE_SHIFT);
    if (!Queue->RingMemory)
        return false;
    memset(Queue->RingMemory, 0, VNET_QUEUE_PAGES * PAGE_SIZE);

    uint32_t DescriptorBytes = Queue->Size * sizeof(VNET_DESCRIPTOR);
    uint32_t AvailableOffset = DescriptorBytes;
    uint32_t AvailableBytes = 4u + Queue->Size * sizeof(uint16_t);
    Queue->UsedOffset = (AvailableOffset + AvailableBytes + PAGE_SIZE - 1u) & ~(PAGE_SIZE - 1u);
    if (Queue->UsedOffset + 4u + Queue->Size * sizeof(VNET_USED_ELEMENT) > VNET_QUEUE_PAGES * PAGE_SIZE)
        return false;
    Queue->Descriptors = (volatile VNET_DESCRIPTOR *)Queue->RingMemory;
    Queue->AvailableIndex = (volatile uint16_t *)(Queue->RingMemory + AvailableOffset + 2u);
    Queue->AvailableRing = (volatile uint16_t *)(Queue->RingMemory + AvailableOffset + 4u);
    Queue->UsedIndex = (volatile uint16_t *)(Queue->RingMemory + Queue->UsedOffset + 2u);
    Queue->UsedRing = (volatile VNET_USED_ELEMENT *)(Queue->RingMemory + Queue->UsedOffset + 4u);
    Queue->AvailableTail = Queue->UsedHead = 0;
    HalWritePort32(Base + 0x08u, (uint32_t)(Queue->RingPfn << PAGE_SHIFT) / PAGE_SIZE);
    return true;
}

static void VnetRelease(VNET_ADAPTER *Adapter)
{
    if (Adapter->Rx.RingPfn != MM_INVALID_PFN)
        MmFreeContiguousPages(Adapter->Rx.RingPfn, VNET_QUEUE_PAGES);
    if (Adapter->Tx.RingPfn != MM_INVALID_PFN)
        MmFreeContiguousPages(Adapter->Tx.RingPfn, VNET_QUEUE_PAGES);
    if (Adapter->RxDataPfn != MM_INVALID_PFN)
        MmFreeContiguousPages(Adapter->RxDataPfn, VNET_BUFFER_PAGES);
    if (Adapter->TxDataPfn != MM_INVALID_PFN)
        MmFreeContiguousPages(Adapter->TxDataPfn, VNET_BUFFER_PAGES);
    ExFreePoolWithTag(Adapter, VNET_TAG);
}

static bool VnetLinkUp(NET_INTERFACE *Interface)
{
    (void)Interface;
    return true; /* The legacy feature set has no negotiated link-status field. */
}

static void VnetPublish(VNET_ADAPTER *Adapter, VNET_QUEUE *Queue, uint16_t QueueIndex, uint16_t Descriptor)
{
    uint16_t Slot = Queue->AvailableTail % Queue->Size;
    Queue->AvailableRing[Slot] = Descriptor;
    __asm__ volatile("sfence" ::: "memory");
    Queue->AvailableTail++;
    *Queue->AvailableIndex = Queue->AvailableTail;
    __asm__ volatile("sfence" ::: "memory");
    HalWritePort16(Adapter->IoBase + 0x10u, QueueIndex);
}

static STATUS VnetSend(NET_INTERFACE *Interface, const void *Frame, uint32_t Length)
{
    VNET_ADAPTER *Adapter = Interface->Context;
    if (!Frame || Length < 14u || Length > Interface->Mtu + 14u)
        return STATUS_INVALID_PARAMETER;
    KeWaitForSingleObject(&Adapter->TxLock, NULL);
    uint8_t *Buffer = Adapter->TxData;
    VNET_HEADER *Header = (VNET_HEADER *)Buffer;
    memset(Header, 0, sizeof(*Header));
    memcpy(Buffer + sizeof(*Header), Frame, Length);
    uint64_t Physical = Adapter->TxDataPfn << PAGE_SHIFT;
    Adapter->Tx.Descriptors[0] = (VNET_DESCRIPTOR){Physical, sizeof(*Header), VNET_DESC_NEXT, 1};
    Adapter->Tx.Descriptors[1] = (VNET_DESCRIPTOR){Physical + sizeof(*Header), Length, 0, 0};
    uint16_t UsedBefore = *Adapter->Tx.UsedIndex;
    VnetPublish(Adapter, &Adapter->Tx, 1, 0);

    bool Completed = false;
    for (uint32_t Elapsed = 0; Elapsed < VNET_TIMEOUT_US; Elapsed += 10u) {
        uint16_t Used = *Adapter->Tx.UsedIndex;
        __asm__ volatile("lfence" ::: "memory");
        if (Used != UsedBefore) {
            VNET_USED_ELEMENT Element = Adapter->Tx.UsedRing[Adapter->Tx.UsedHead % Adapter->Tx.Size];
            Adapter->Tx.UsedHead++;
            Completed = Element.Id == 0;
            break;
        }
        KeStallExecutionProcessor(10);
    }
    KeReleaseMutex(&Adapter->TxLock);
    return Completed ? STATUS_SUCCESS : STATUS_IO_TIMEOUT;
}

static STATUS VnetReceive(NET_INTERFACE *Interface, void *Frame, uint32_t Capacity, uint32_t *Length)
{
    VNET_ADAPTER *Adapter = Interface->Context;
    if (!Frame || !Length)
        return STATUS_INVALID_PARAMETER;
    *Length = 0;
    KeWaitForSingleObject(&Adapter->RxLock, NULL);
    uint16_t Used = *Adapter->Rx.UsedIndex;
    __asm__ volatile("lfence" ::: "memory");
    if (Used == Adapter->Rx.UsedHead) {
        KeReleaseMutex(&Adapter->RxLock);
        return STATUS_NO_MORE_ENTRIES;
    }
    VNET_USED_ELEMENT Element = Adapter->Rx.UsedRing[Adapter->Rx.UsedHead % Adapter->Rx.Size];
    Adapter->Rx.UsedHead++;
    STATUS Result = STATUS_SUCCESS;
    if (Element.Id >= VNET_BUFFER_COUNT || Element.Length < sizeof(VNET_HEADER) + 14u ||
        Element.Length > VNET_BUFFER_SIZE) {
        Result = STATUS_DEVICE_DATA_ERROR;
    } else {
        uint32_t FrameLength = Element.Length - sizeof(VNET_HEADER);
        if (Capacity < FrameLength) {
            *Length = FrameLength;
            Result = STATUS_BUFFER_TOO_SMALL;
        } else {
            memcpy(Frame, Adapter->RxData + Element.Id * VNET_BUFFER_SIZE + sizeof(VNET_HEADER), FrameLength);
            *Length = FrameLength;
        }
        Adapter->Rx.Descriptors[Element.Id].Length = VNET_BUFFER_SIZE;
        Adapter->Rx.Descriptors[Element.Id].Flags = VNET_DESC_DEVICE_WRITE;
        Adapter->Rx.Descriptors[Element.Id].Next = 0;
        VnetPublish(Adapter, &Adapter->Rx, 0, (uint16_t)Element.Id);
    }
    KeReleaseMutex(&Adapter->RxLock);
    return Result;
}

static bool VnetStart(VNET_ADAPTER *Adapter)
{
    uint16_t Base = Adapter->IoBase;
    HalWritePort8(Base + 0x12u, 0);
    HalWritePort8(Base + 0x12u, VNET_STATUS_ACKNOWLEDGE);
    HalWritePort8(Base + 0x12u, VNET_STATUS_ACKNOWLEDGE | VNET_STATUS_DRIVER);
    if (!(HalReadPort32(Base) & VNET_FEATURE_MAC))
        goto Failure;
    HalWritePort32(Base + 0x04u, VNET_FEATURE_MAC);
    for (uint32_t I = 0; I < 6; I++)
        Adapter->Interface.MacAddress[I] = HalReadPort8(Base + 0x14u + I);
    if ((Adapter->Interface.MacAddress[0] & 1u) ||
        !(Adapter->Interface.MacAddress[0] | Adapter->Interface.MacAddress[1] |
          Adapter->Interface.MacAddress[2] | Adapter->Interface.MacAddress[3] |
          Adapter->Interface.MacAddress[4] | Adapter->Interface.MacAddress[5]))
        goto Failure;

    if (!HZ_SUCCESS(MmAllocateContiguousPages(VNET_BUFFER_PAGES, VNET_DMA_LIMIT, 1, &Adapter->RxDataPfn)) ||
        !HZ_SUCCESS(MmAllocateContiguousPages(VNET_BUFFER_PAGES, VNET_DMA_LIMIT, 1, &Adapter->TxDataPfn)))
        goto Failure;
    Adapter->RxData = MmPhysicalToVirtual(Adapter->RxDataPfn << PAGE_SHIFT);
    Adapter->TxData = MmPhysicalToVirtual(Adapter->TxDataPfn << PAGE_SHIFT);
    if (!Adapter->RxData || !Adapter->TxData)
        goto Failure;
    memset(Adapter->RxData, 0, VNET_BUFFER_PAGES * PAGE_SIZE);
    memset(Adapter->TxData, 0, VNET_BUFFER_PAGES * PAGE_SIZE);
    Adapter->Rx.RingPfn = Adapter->Tx.RingPfn = MM_INVALID_PFN;
    if (!VnetInitializeQueue(Adapter, &Adapter->Rx, 0) || !VnetInitializeQueue(Adapter, &Adapter->Tx, 1))
        goto Failure;

    for (uint16_t I = 0; I < VNET_BUFFER_COUNT; I++) {
        uint64_t Physical = (Adapter->RxDataPfn << PAGE_SHIFT) + (uint64_t)I * VNET_BUFFER_SIZE;
        Adapter->Rx.Descriptors[I] = (VNET_DESCRIPTOR){Physical, VNET_BUFFER_SIZE, VNET_DESC_DEVICE_WRITE, 0};
        VnetPublish(Adapter, &Adapter->Rx, 0, I);
    }
    HalWritePort8(Base + 0x12u, VNET_STATUS_ACKNOWLEDGE | VNET_STATUS_DRIVER | VNET_STATUS_DRIVER_OK);
    return true;

Failure:
    HalWritePort8(Base + 0x12u, VNET_STATUS_FAILED);
    return false;
}

static STATUS VnetDriverEntry(DRIVER_OBJECT *Driver)
{
    (void)Driver;
    uint32_t Started = 0;
    for (PCI_DEVICE *Pci = PciGetDeviceList(); Pci; Pci = Pci->Next) {
        if (Pci->VendorId != VNET_VENDOR || Pci->DeviceId != VNET_DEVICE)
            continue;
        const char *Model = "Nexora VirtIO network (legacy PCI)";
        if (!Pci->Bar[0] || !Pci->BarIsIo[0] || Pci->BarSize[0] < 0x20u) {
            PciSetDriverStatus(Pci, PciDriverStartFailed, Model);
            continue;
        }
        VNET_ADAPTER *Adapter = ExAllocatePoolZero(NonPagedPool, sizeof(*Adapter), VNET_TAG);
        if (!Adapter) {
            PciSetDriverStatus(Pci, PciDriverStartFailed, Model);
            continue;
        }
        Adapter->Pci = Pci;
        Adapter->IoBase = (uint16_t)Pci->Bar[0];
        Adapter->Rx.RingPfn = Adapter->Tx.RingPfn = MM_INVALID_PFN;
        Adapter->RxDataPfn = Adapter->TxDataPfn = MM_INVALID_PFN;
        PciEnableDevice(Pci, PCI_COMMAND_IO | PCI_COMMAND_BUS_MASTER);
        KeInitializeMutex(&Adapter->RxLock);
        KeInitializeMutex(&Adapter->TxLock);
        Adapter->Interface.Context = Adapter;
        Adapter->Interface.Mtu = 1500;
        Adapter->Interface.SendFrame = VnetSend;
        Adapter->Interface.ReceiveFrame = VnetReceive;
        Adapter->Interface.QueryLink = VnetLinkUp;
        RtlFormatString(Adapter->Interface.Name, sizeof(Adapter->Interface.Name), "eth%u", Started);
        if (!VnetStart(Adapter)) {
            VnetRelease(Adapter);
            PciSetDriverStatus(Pci, PciDriverStartFailed, Model);
            continue;
        }
        if (!HZ_SUCCESS(NetRegisterInterface(&Adapter->Interface))) {
            HalWritePort8(Adapter->IoBase + 0x12u, VNET_STATUS_FAILED);
            VnetRelease(Adapter);
            PciSetDriverStatus(Pci, PciDriverStartFailed, Model);
            continue;
        }
        PciSetDriverStatus(Pci, PciDriverStarted, Model);
        DbgPrint("VirtIO network: %02x:%02x.%u, MAC %02x:%02x:%02x:%02x:%02x:%02x, queues %u/%u\n",
                 Pci->Bus, Pci->Device, Pci->Function, Adapter->Interface.MacAddress[0],
                 Adapter->Interface.MacAddress[1], Adapter->Interface.MacAddress[2],
                 Adapter->Interface.MacAddress[3], Adapter->Interface.MacAddress[4],
                 Adapter->Interface.MacAddress[5], Adapter->Rx.Size, Adapter->Tx.Size);
        Started++;
    }
    return Started ? STATUS_SUCCESS : STATUS_NO_SUCH_DEVICE;
}

IO_DECLARE_BUILTIN_DRIVER(VirtioNetworkBuiltinDriver, "\\Driver\\VirtioNetwork", VnetDriverEntry,
                          IO_ORDER_DEVICE);
