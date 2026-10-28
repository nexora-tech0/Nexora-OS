/*
 * Nexora OS - drivers/virtio/virtio_blk.c
 * Legacy/transitional PCI VirtIO block device (1AF4:1001). The driver negotiates no optional
 * features, uses a single synchronous three-descriptor request chain, and exposes 512-byte
 * sectors through the common disk class. Modern VirtIO PCI capabilities are not handled here.
 */
#include "disk.h"
#include "ex.h"
#include "hal.h"
#include "io.h"
#include "ke.h"
#include "mm.h"
#include "pci.h"
#include "rtl.h"

#define VBLK_TAG HZ_POOL_TAG('V', 'B', 'l', 'k')
#define VBLK_VENDOR 0x1AF4u
#define VBLK_DEVICE 0x1001u
#define VBLK_QUEUE_PAGES 8u
#define VBLK_DATA_PAGES 1u
#define VBLK_DMA_LIMIT 0x100000000ull
#define VBLK_TIMEOUT_US 5000000u
#define VBLK_DESC_NEXT 1u
#define VBLK_DESC_DEVICE_WRITE 2u
#define VBLK_TYPE_IN 0u
#define VBLK_TYPE_OUT 1u
#define VBLK_STATUS_OK 0u
#define VBLK_STATUS_IOERR 1u
#define VBLK_STATUS_UNSUPP 2u
#define VBLK_STATUS_ACKNOWLEDGE 1u
#define VBLK_STATUS_DRIVER 2u
#define VBLK_STATUS_DRIVER_OK 4u
#define VBLK_STATUS_FAILED 128u

typedef struct HZ_PACKED _VBLK_DESCRIPTOR {
    uint64_t Address;
    uint32_t Length;
    uint16_t Flags;
    uint16_t Next;
} VBLK_DESCRIPTOR;

typedef struct HZ_PACKED _VBLK_USED_ELEMENT {
    uint32_t Id;
    uint32_t Length;
} VBLK_USED_ELEMENT;

typedef struct HZ_PACKED _VBLK_REQUEST_HEADER {
    uint32_t Type;
    uint32_t Reserved;
    uint64_t Sector;
} VBLK_REQUEST_HEADER;

typedef struct _VBLK_ADAPTER {
    PCI_DEVICE *Pci;
    uint16_t IoBase;
    PFN_NUMBER QueuePfn;
    PFN_NUMBER DataPfn;
    uint8_t *QueueMemory;
    uint8_t *Data;
    volatile VBLK_DESCRIPTOR *Descriptors;
    volatile uint16_t *AvailableIndex;
    volatile uint16_t *AvailableRing;
    volatile uint16_t *UsedIndex;
    volatile VBLK_USED_ELEMENT *UsedRing;
    uint32_t UsedOffset;
    uint16_t QueueSize;
    uint16_t LastUsed;
    uint64_t SectorCount;
    KMUTEX TransferLock;
} VBLK_ADAPTER;

static uint32_t VblkAlignUp(uint32_t Value, uint32_t Alignment)
{
    return (Value + Alignment - 1u) & ~(Alignment - 1u);
}

static void VblkRelease(VBLK_ADAPTER *Adapter)
{
    if (Adapter->QueuePfn != MM_INVALID_PFN)
        MmFreeContiguousPages(Adapter->QueuePfn, VBLK_QUEUE_PAGES);
    if (Adapter->DataPfn != MM_INVALID_PFN)
        MmFreeContiguousPages(Adapter->DataPfn, VBLK_DATA_PAGES);
    ExFreePoolWithTag(Adapter, VBLK_TAG);
}

static STATUS VblkTransfer(void *Context, uint64_t Lba, uint32_t Count, void *Buffer, bool Write)
{
    VBLK_ADAPTER *Adapter = Context;
    if (!Buffer || !Count || Count > (PAGE_SIZE - sizeof(VBLK_REQUEST_HEADER) - 1u) / 512u ||
        Lba >= Adapter->SectorCount ||
        Count > Adapter->SectorCount - Lba)
        return STATUS_INVALID_PARAMETER;
    KeWaitForSingleObject(&Adapter->TransferLock, NULL);

    VBLK_REQUEST_HEADER *Header = (VBLK_REQUEST_HEADER *)Adapter->Data;
    uint8_t *Payload = Adapter->Data + sizeof(*Header);
    uint8_t *DeviceStatus = Payload + Count * 512u;
    Header->Type = Write ? VBLK_TYPE_OUT : VBLK_TYPE_IN;
    Header->Reserved = 0;
    Header->Sector = Lba;
    *DeviceStatus = 0xFFu;
    if (Write)
        memcpy(Payload, Buffer, Count * 512u);

    uint64_t Dma = Adapter->DataPfn << PAGE_SHIFT;
    Adapter->Descriptors[0] = (VBLK_DESCRIPTOR){Dma, sizeof(*Header), VBLK_DESC_NEXT, 1};
    Adapter->Descriptors[1] = (VBLK_DESCRIPTOR){Dma + sizeof(*Header), Count * 512u,
                                                   VBLK_DESC_NEXT | (Write ? 0u : VBLK_DESC_DEVICE_WRITE), 2};
    Adapter->Descriptors[2] = (VBLK_DESCRIPTOR){Dma + sizeof(*Header) + Count * 512u, 1,
                                                   VBLK_DESC_DEVICE_WRITE, 0};

    uint16_t Slot = *Adapter->AvailableIndex % Adapter->QueueSize;
    Adapter->AvailableRing[Slot] = 0;
    __asm__ volatile("sfence" ::: "memory");
    *Adapter->AvailableIndex = (uint16_t)(*Adapter->AvailableIndex + 1u);
    __asm__ volatile("sfence" ::: "memory");
    HalWritePort16(Adapter->IoBase + 0x10u, 0);

    bool Complete = false;
    bool SawCompletion = false;
    for (uint32_t Elapsed = 0; Elapsed < VBLK_TIMEOUT_US; Elapsed += 10u) {
        uint16_t Used = *Adapter->UsedIndex;
        __asm__ volatile("lfence" ::: "memory");
        if (Used != Adapter->LastUsed) {
            VBLK_USED_ELEMENT Element = Adapter->UsedRing[Adapter->LastUsed % Adapter->QueueSize];
            Adapter->LastUsed++;
            SawCompletion = true;
            Complete = Element.Id == 0 && *DeviceStatus == VBLK_STATUS_OK;
            break;
        }
        KeStallExecutionProcessor(10);
    }

    STATUS Status = Complete ? STATUS_SUCCESS : STATUS_DEVICE_DATA_ERROR;
    if (!SawCompletion)
        Status = STATUS_IO_TIMEOUT;
    if (Complete && !Write)
        memcpy(Buffer, Payload, Count * 512u);
    KeReleaseMutex(&Adapter->TransferLock);
    return Status;
}

static bool VblkStart(VBLK_ADAPTER *Adapter)
{
    uint16_t Base = Adapter->IoBase;
    HalWritePort8(Base + 0x12u, 0);
    HalWritePort8(Base + 0x12u, VBLK_STATUS_ACKNOWLEDGE);
    HalWritePort8(Base + 0x12u, VBLK_STATUS_ACKNOWLEDGE | VBLK_STATUS_DRIVER);
    /* No optional features are requested: in particular, retain the legacy 512-byte sector ABI. */
    HalWritePort32(Base + 0x04u, 0);
    HalWritePort16(Base + 0x0Eu, 0);
    uint16_t Maximum = HalReadPort16(Base + 0x0Cu);
    if (Maximum < 8u)
        goto Failure;
    /* Legacy PCI exposes the device's fixed queue size as read-only. Keep that exact size: ring
       offsets and the PFN registration are defined from it by the device. */
    if (Maximum > 1024u || (Maximum & (Maximum - 1u)) != 0)
        goto Failure;
    Adapter->QueueSize = Maximum;

    if (!HZ_SUCCESS(MmAllocateContiguousPages(VBLK_QUEUE_PAGES, VBLK_DMA_LIMIT, 1, &Adapter->QueuePfn)) ||
        !HZ_SUCCESS(MmAllocateContiguousPages(VBLK_DATA_PAGES, VBLK_DMA_LIMIT, 1, &Adapter->DataPfn)))
        goto Failure;
    Adapter->QueueMemory = MmPhysicalToVirtual(Adapter->QueuePfn << PAGE_SHIFT);
    Adapter->Data = MmPhysicalToVirtual(Adapter->DataPfn << PAGE_SHIFT);
    if (!Adapter->QueueMemory || !Adapter->Data)
        goto Failure;
    memset(Adapter->QueueMemory, 0, VBLK_QUEUE_PAGES * PAGE_SIZE);
    memset(Adapter->Data, 0, VBLK_DATA_PAGES * PAGE_SIZE);

    uint32_t DescriptorBytes = Adapter->QueueSize * sizeof(VBLK_DESCRIPTOR);
    uint32_t AvailableOffset = DescriptorBytes;
    uint32_t AvailableBytes = 4u + Adapter->QueueSize * sizeof(uint16_t);
    Adapter->UsedOffset = VblkAlignUp(AvailableOffset + AvailableBytes, PAGE_SIZE);
    if (Adapter->UsedOffset + 4u + Adapter->QueueSize * sizeof(VBLK_USED_ELEMENT) >
        VBLK_QUEUE_PAGES * PAGE_SIZE)
        goto Failure;
    Adapter->Descriptors = (volatile VBLK_DESCRIPTOR *)Adapter->QueueMemory;
    Adapter->AvailableIndex = (volatile uint16_t *)(Adapter->QueueMemory + AvailableOffset + 2u);
    Adapter->AvailableRing = (volatile uint16_t *)(Adapter->QueueMemory + AvailableOffset + 4u);
    Adapter->UsedIndex = (volatile uint16_t *)(Adapter->QueueMemory + Adapter->UsedOffset + 2u);
    Adapter->UsedRing = (volatile VBLK_USED_ELEMENT *)(Adapter->QueueMemory + Adapter->UsedOffset + 4u);
    Adapter->LastUsed = 0;

    HalWritePort16(Base + 0x0Eu, 0);
    HalWritePort32(Base + 0x08u, (uint32_t)(Adapter->QueuePfn << PAGE_SHIFT) / PAGE_SIZE);
    uint8_t Status = HalReadPort8(Base + 0x12u);
    HalWritePort8(Base + 0x12u, Status | VBLK_STATUS_DRIVER_OK);

    uint32_t CapacityLow = HalReadPort32(Base + 0x14u);
    uint32_t CapacityHigh = HalReadPort32(Base + 0x18u);
    Adapter->SectorCount = ((uint64_t)CapacityHigh << 32) | CapacityLow;
    return Adapter->SectorCount != 0;

Failure:
    HalWritePort8(Base + 0x12u, VBLK_STATUS_FAILED);
    return false;
}

static STATUS VblkDriverEntry(DRIVER_OBJECT *Driver)
{
    (void)Driver;
    uint32_t Started = 0;
    for (PCI_DEVICE *Pci = PciGetDeviceList(); Pci; Pci = Pci->Next) {
        if (Pci->VendorId != VBLK_VENDOR || Pci->DeviceId != VBLK_DEVICE)
            continue;
        const char *Model = "Nexora VirtIO block storage";
        if (!Pci->Bar[0] || !Pci->BarIsIo[0] || Pci->BarSize[0] < 0x20u) {
            PciSetDriverStatus(Pci, PciDriverStartFailed, Model);
            continue;
        }
        VBLK_ADAPTER *Adapter = ExAllocatePoolZero(NonPagedPool, sizeof(*Adapter), VBLK_TAG);
        if (!Adapter) {
            PciSetDriverStatus(Pci, PciDriverStartFailed, Model);
            continue;
        }
        Adapter->Pci = Pci;
        Adapter->IoBase = (uint16_t)Pci->Bar[0];
        Adapter->QueuePfn = Adapter->DataPfn = MM_INVALID_PFN;
        PciEnableDevice(Pci, PCI_COMMAND_IO | PCI_COMMAND_BUS_MASTER);
        KeInitializeMutex(&Adapter->TransferLock);
        if (!VblkStart(Adapter)) {
            if (Adapter->QueuePfn != MM_INVALID_PFN || Adapter->DataPfn != MM_INVALID_PFN)
                VblkRelease(Adapter);
            else
                ExFreePoolWithTag(Adapter, VBLK_TAG);
            PciSetDriverStatus(Pci, PciDriverStartFailed, Model);
            continue;
        }
        DISK_DESCRIPTION Disk;
        memset(&Disk, 0, sizeof(Disk));
        RtlStringCopy(Disk.Model, sizeof(Disk.Model), "VirtIO block device");
        Disk.SectorSize = 512;
        Disk.SectorCount = Adapter->SectorCount;
        Disk.MaxTransferSectors = (PAGE_SIZE - sizeof(VBLK_REQUEST_HEADER) - 1u) / 512u;
        Disk.Transfer = VblkTransfer;
        Disk.Context = Adapter;
        uint32_t DiskNumber;
        if (!HZ_SUCCESS(DiskCreateDisk(&Disk, &DiskNumber))) {
            HalWritePort8(Adapter->IoBase + 0x12u, VBLK_STATUS_FAILED);
            VblkRelease(Adapter);
            PciSetDriverStatus(Pci, PciDriverStartFailed, Model);
            continue;
        }
        PciSetDriverStatus(Pci, PciDriverStarted, Model);
        DbgPrint("VirtIO block: %02x:%02x.%u, capacity %llu sectors, queue %u (PCI %04x:%04x)\n",
                 Pci->Bus, Pci->Device, Pci->Function, (unsigned long long)Adapter->SectorCount,
                 Adapter->QueueSize, Pci->VendorId, Pci->DeviceId);
        Started++;
    }
    return Started ? STATUS_SUCCESS : STATUS_NO_SUCH_DEVICE;
}

IO_DECLARE_BUILTIN_DRIVER(VirtioBlockBuiltinDriver, "\\Driver\\VirtioBlock", VblkDriverEntry,
                          IO_ORDER_DEVICE);
