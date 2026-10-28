/*
 * Nexora OS - drivers/nvme/nvme.c
 * Minimal polling NVMe driver for PCI Express NVM Express controllers.
 *
 * This is a native Nexora driver. It creates admin and I/O queues, identifies the first active
 * namespace, and exposes it through the common disk class. Transfers use a physically contiguous
 * bounce area and PRP entries, so callers may pass ordinary kernel buffers. MSI is deliberately
 * not required for boot or Setup: completions are polled with a bounded timeout.
 */
#include "disk.h"
#include "ex.h"
#include "io.h"
#include "mm.h"
#include "pci.h"
#include "rtl.h"

#define NVME_TAG HZ_POOL_TAG('N', 'v', 'm', 'e')
#define NVME_PAGE_SIZE 4096u
#define NVME_QUEUE_DEPTH 16u
#define NVME_DMA_PAGES 24u
#define NVME_BOUNCE_FIRST_PAGE 5u
#define NVME_BOUNCE_PAGES 16u
#define NVME_PRP_PAGE 21u
#define NVME_BOUNCE_BYTES (NVME_BOUNCE_PAGES * NVME_PAGE_SIZE)
#define NVME_TIMEOUT_US 5000000u

#define NVME_REG_CAP 0x0000u
#define NVME_REG_VS 0x0008u
#define NVME_REG_INTMS 0x000Cu
#define NVME_REG_CC 0x0014u
#define NVME_REG_CSTS 0x001Cu
#define NVME_REG_AQA 0x0024u
#define NVME_REG_ASQ 0x0028u
#define NVME_REG_ACQ 0x0030u
#define NVME_REG_DOORBELL 0x1000u

#define NVME_CC_EN (1u << 0)
#define NVME_CSTS_RDY (1u << 0)
#define NVME_CSTS_CFS (1u << 1)
#define NVME_SQES_64 (6u << 16)
#define NVME_CQES_16 (4u << 20)
#define NVME_ADMIN_IDENTIFY 0x06u
#define NVME_ADMIN_CREATE_IO_CQ 0x05u
#define NVME_ADMIN_CREATE_IO_SQ 0x01u
#define NVME_NVM_READ 0x02u
#define NVME_NVM_WRITE 0x01u
#define NVME_IDENTIFY_CONTROLLER 1u
#define NVME_IDENTIFY_ACTIVE_NS_LIST 2u
#define NVME_IDENTIFY_NAMESPACE 0u

typedef struct HZ_PACKED _NVME_COMMAND {
    uint8_t Opcode;
    uint8_t Flags;
    uint16_t CommandId;
    uint32_t NamespaceId;
    uint64_t Reserved;
    uint64_t MetadataPointer;
    uint64_t Prp1;
    uint64_t Prp2;
    uint32_t CommandDword10;
    uint32_t CommandDword11;
    uint32_t CommandDword12;
    uint32_t CommandDword13;
    uint32_t CommandDword14;
    uint32_t CommandDword15;
} NVME_COMMAND;

typedef struct HZ_PACKED _NVME_COMPLETION {
    uint32_t Result;
    uint32_t Reserved;
    uint16_t SubmissionHead;
    uint16_t SubmissionQueueId;
    uint16_t CommandId;
    uint16_t Status;
} NVME_COMPLETION;

typedef struct _NVME_QUEUE {
    volatile NVME_COMMAND *Submission;
    volatile NVME_COMPLETION *Completion;
    uint16_t SubmissionTail;
    uint16_t CompletionHead;
    uint16_t NextCommandId;
    uint8_t CompletionPhase;
    volatile uint32_t *SubmissionDoorbell;
    volatile uint32_t *CompletionDoorbell;
} NVME_QUEUE;

typedef struct _NVME_CONTROLLER {
    PCI_DEVICE *Pci;
    volatile uint8_t *Registers;
    uint32_t DoorbellStride;
    PFN_NUMBER DmaPfn;
    uint8_t *Dma;
    NVME_QUEUE Admin;
    NVME_QUEUE Io;
    KMUTEX TransferLock;
    uint32_t NamespaceId;
    uint32_t SectorSize;
    uint64_t SectorCount;
    uint32_t MaxTransferSectors;
    char Model[41];
} NVME_CONTROLLER;

_Static_assert(sizeof(NVME_COMMAND) == 64, "NVMe command size");
_Static_assert(sizeof(NVME_COMPLETION) == 16, "NVMe completion size");

static uint32_t NvmeRead32(NVME_CONTROLLER *Controller, uint32_t Offset)
{
    return *(volatile uint32_t *)(Controller->Registers + Offset);
}

static void NvmeWrite32(NVME_CONTROLLER *Controller, uint32_t Offset, uint32_t Value)
{
    *(volatile uint32_t *)(Controller->Registers + Offset) = Value;
}

static void NvmeWrite64(NVME_CONTROLLER *Controller, uint32_t Offset, uint64_t Value)
{
    *(volatile uint64_t *)(Controller->Registers + Offset) = Value;
}

static bool NvmeWaitReady(NVME_CONTROLLER *Controller, bool Ready)
{
    for (uint32_t Elapsed = 0; Elapsed < NVME_TIMEOUT_US; Elapsed += 100) {
        uint32_t Status = NvmeRead32(Controller, NVME_REG_CSTS);
        if (Status & NVME_CSTS_CFS)
            return false;
        if (((Status & NVME_CSTS_RDY) != 0) == Ready)
            return true;
        KeStallExecutionProcessor(100);
    }
    return false;
}

static uint32_t NvmeDoorbellOffset(NVME_CONTROLLER *Controller, uint32_t QueueId, bool Completion)
{
    return NVME_REG_DOORBELL + ((QueueId * 2u + (Completion ? 1u : 0u)) * Controller->DoorbellStride);
}

static void NvmeInitializeQueue(NVME_CONTROLLER *Controller, NVME_QUEUE *Queue, uint32_t QueueId,
                                uint32_t SubmissionPage, uint32_t CompletionPage)
{
    Queue->Submission = (volatile NVME_COMMAND *)(Controller->Dma + SubmissionPage * NVME_PAGE_SIZE);
    Queue->Completion = (volatile NVME_COMPLETION *)(Controller->Dma + CompletionPage * NVME_PAGE_SIZE);
    Queue->SubmissionDoorbell = (volatile uint32_t *)(Controller->Registers +
                                  NvmeDoorbellOffset(Controller, QueueId, false));
    Queue->CompletionDoorbell = (volatile uint32_t *)(Controller->Registers +
                                   NvmeDoorbellOffset(Controller, QueueId, true));
    Queue->SubmissionTail = 0;
    Queue->CompletionHead = 0;
    Queue->NextCommandId = 1;
    Queue->CompletionPhase = 1;
}

static STATUS NvmeSubmit(NVME_QUEUE *Queue, NVME_COMMAND *Command, uint32_t *Result)
{
    uint16_t Slot = Queue->SubmissionTail;
    uint16_t CommandId = Queue->NextCommandId++;
    if (!Queue->NextCommandId)
        Queue->NextCommandId = 1;
    Command->CommandId = CommandId;
    memcpy((void *)&Queue->Submission[Slot], Command, sizeof(*Command));
    __asm__ volatile("sfence" ::: "memory");
    Queue->SubmissionTail = (uint16_t)((Slot + 1u) % NVME_QUEUE_DEPTH);
    *Queue->SubmissionDoorbell = Queue->SubmissionTail;

    for (uint32_t Elapsed = 0; Elapsed < NVME_TIMEOUT_US; Elapsed += 10) {
        NVME_COMPLETION Completion;
        memcpy(&Completion, (const void *)&Queue->Completion[Queue->CompletionHead], sizeof(Completion));
        __asm__ volatile("lfence" ::: "memory");
        if ((Completion.Status & 1u) == Queue->CompletionPhase) {
            Queue->CompletionHead++;
            if (Queue->CompletionHead == NVME_QUEUE_DEPTH) {
                Queue->CompletionHead = 0;
                Queue->CompletionPhase ^= 1u;
            }
            *Queue->CompletionDoorbell = Queue->CompletionHead;
            if (Completion.CommandId != CommandId || (Completion.Status & 0xFFFEu) != 0)
                return STATUS_DEVICE_DATA_ERROR;
            if (Result)
                *Result = Completion.Result;
            return STATUS_SUCCESS;
        }
        KeStallExecutionProcessor(10);
    }
    return STATUS_IO_TIMEOUT;
}

static uint64_t NvmeDmaAddress(NVME_CONTROLLER *Controller, uint32_t Page)
{
    return ((uint64_t)Controller->DmaPfn + Page) << PAGE_SHIFT;
}

static STATUS NvmeIdentify(NVME_CONTROLLER *Controller, uint32_t NamespaceId, uint32_t Cns)
{
    NVME_COMMAND Command;
    memset(&Command, 0, sizeof(Command));
    Command.Opcode = NVME_ADMIN_IDENTIFY;
    Command.NamespaceId = NamespaceId;
    Command.Prp1 = NvmeDmaAddress(Controller, 4);
    Command.CommandDword10 = Cns;
    return NvmeSubmit(&Controller->Admin, &Command, NULL);
}

static STATUS NvmeTransfer(void *Context, uint64_t Lba, uint32_t Count, void *Buffer, bool Write)
{
    NVME_CONTROLLER *Controller = Context;
    uint64_t Bytes = (uint64_t)Count * Controller->SectorSize;
    if (!Count || Count > Controller->MaxTransferSectors || Lba >= Controller->SectorCount ||
        Count > Controller->SectorCount - Lba || Bytes > NVME_BOUNCE_BYTES)
        return STATUS_INVALID_PARAMETER;

    KeWaitForSingleObject(&Controller->TransferLock, NULL);
    uint8_t *Bounce = Controller->Dma + NVME_BOUNCE_FIRST_PAGE * NVME_PAGE_SIZE;
    if (Write)
        memcpy(Bounce, Buffer, (size_t)Bytes);

    uint32_t PageCount = (uint32_t)((Bytes + NVME_PAGE_SIZE - 1u) / NVME_PAGE_SIZE);
    uint64_t DataPhysical = NvmeDmaAddress(Controller, NVME_BOUNCE_FIRST_PAGE);
    uint64_t Prp2 = 0;
    if (PageCount == 2) {
        Prp2 = DataPhysical + NVME_PAGE_SIZE;
    } else if (PageCount > 2) {
        uint64_t *PrpList = (uint64_t *)(Controller->Dma + NVME_PRP_PAGE * NVME_PAGE_SIZE);
        for (uint32_t i = 1; i < PageCount; i++)
            PrpList[i - 1] = DataPhysical + (uint64_t)i * NVME_PAGE_SIZE;
        Prp2 = NvmeDmaAddress(Controller, NVME_PRP_PAGE);
    }

    NVME_COMMAND Command;
    memset(&Command, 0, sizeof(Command));
    Command.Opcode = Write ? NVME_NVM_WRITE : NVME_NVM_READ;
    Command.NamespaceId = Controller->NamespaceId;
    Command.Prp1 = DataPhysical;
    Command.Prp2 = Prp2;
    Command.CommandDword10 = (uint32_t)Lba;
    Command.CommandDword11 = (uint32_t)(Lba >> 32);
    Command.CommandDword12 = Count - 1u;
    STATUS Status = NvmeSubmit(&Controller->Io, &Command, NULL);
    if (HZ_SUCCESS(Status) && !Write)
        memcpy(Buffer, Bounce, (size_t)Bytes);
    KeReleaseMutex(&Controller->TransferLock);
    return Status;
}

static bool NvmeInitializeController(PCI_DEVICE *Pci)
{
    if (!Pci->Bar[0] || Pci->BarIsIo[0] || Pci->BarSize[0] < 0x2000) {
        DbgPrintEx(DPFLTR_WARNING_LEVEL, "NVMe: %04x:%04x has invalid BAR0 (0x%llx/0x%llx)\n",
                   Pci->VendorId, Pci->DeviceId, (unsigned long long)Pci->Bar[0],
                   (unsigned long long)Pci->BarSize[0]);
        return false;
    }

    NVME_CONTROLLER *Controller = ExAllocatePoolZero(NonPagedPool, sizeof(*Controller), NVME_TAG);
    if (!Controller)
        return false;
    Controller->Pci = Pci;
    Controller->Registers = MmMapIoSpace(Pci->Bar[0], (size_t)Pci->BarSize[0], MM_CACHE_UNCACHED);
    if (!Controller->Registers) {
        DbgPrintEx(DPFLTR_WARNING_LEVEL, "NVMe: cannot map BAR0 at 0x%llx (%llu bytes)\n",
                   (unsigned long long)Pci->Bar[0], (unsigned long long)Pci->BarSize[0]);
        ExFreePoolWithTag(Controller, NVME_TAG);
        return false;
    }

    uint64_t Capability = *(volatile uint64_t *)(Controller->Registers + NVME_REG_CAP);
    uint32_t MinPageSize = (uint32_t)((Capability >> 48) & 0xFu);
    uint32_t MaxPageSize = (uint32_t)((Capability >> 52) & 0xFu);
    if (MinPageSize != 0 || MaxPageSize < MinPageSize ||
        (Capability & 0xFFFFu) + 1u < NVME_QUEUE_DEPTH) {
        DbgPrintEx(DPFLTR_WARNING_LEVEL, "NVMe: unsupported CAP 0x%016llx (MPS %u..%u, MQES %u)\n",
                   (unsigned long long)Capability, MinPageSize, MaxPageSize,
                   (uint32_t)(Capability & 0xFFFFu) + 1u);
        ExFreePoolWithTag(Controller, NVME_TAG);
        return false;
    }
    Controller->DoorbellStride = 4u << ((Capability >> 32) & 0xFu);
    if (NvmeDoorbellOffset(Controller, 1, true) + sizeof(uint32_t) > Pci->BarSize[0]) {
        DbgPrintEx(DPFLTR_WARNING_LEVEL, "NVMe: BAR0 too small for doorbells (need 0x%x, have 0x%llx)\n",
                   NvmeDoorbellOffset(Controller, 1, true) + (uint32_t)sizeof(uint32_t),
                   (unsigned long long)Pci->BarSize[0]);
        ExFreePoolWithTag(Controller, NVME_TAG);
        return false;
    }

    uint32_t Version = NvmeRead32(Controller, NVME_REG_VS);
    if (Version == 0 || Version == 0xFFFFFFFFu) {
        DbgPrintEx(DPFLTR_WARNING_LEVEL, "NVMe: invalid version register 0x%08x\n", Version);
        ExFreePoolWithTag(Controller, NVME_TAG);
        return false;
    }

    PFN_NUMBER DmaPfn;
    if (!HZ_SUCCESS(MmAllocateContiguousPages(NVME_DMA_PAGES, ~0ull, 1, &DmaPfn))) {
        DbgPrintEx(DPFLTR_WARNING_LEVEL, "NVMe: could not allocate %u contiguous DMA pages\n", NVME_DMA_PAGES);
        ExFreePoolWithTag(Controller, NVME_TAG);
        return false;
    }
    Controller->DmaPfn = DmaPfn;
    Controller->Dma = MmPfnToVirtual(DmaPfn);
    memset(Controller->Dma, 0, NVME_DMA_PAGES * NVME_PAGE_SIZE);
    const char *FailureStage = "controller reset";

    KeInitializeMutex(&Controller->TransferLock);
    PciEnableDevice(Pci, PCI_COMMAND_MEMORY | PCI_COMMAND_BUS_MASTER);

    NvmeWrite32(Controller, NVME_REG_CC, 0);
    if (!NvmeWaitReady(Controller, false))
        goto Failure;

    Controller->Admin.Submission = (volatile NVME_COMMAND *)(Controller->Dma + 0 * NVME_PAGE_SIZE);
    Controller->Admin.Completion = (volatile NVME_COMPLETION *)(Controller->Dma + 1 * NVME_PAGE_SIZE);
    Controller->Admin.SubmissionDoorbell = (volatile uint32_t *)(Controller->Registers +
                                         NvmeDoorbellOffset(Controller, 0, false));
    Controller->Admin.CompletionDoorbell = (volatile uint32_t *)(Controller->Registers +
                                         NvmeDoorbellOffset(Controller, 0, true));
    Controller->Admin.CompletionPhase = 1;
    Controller->Admin.NextCommandId = 1;

    NvmeWrite32(Controller, NVME_REG_AQA, ((NVME_QUEUE_DEPTH - 1u) << 16) | (NVME_QUEUE_DEPTH - 1u));
    NvmeWrite64(Controller, NVME_REG_ASQ, NvmeDmaAddress(Controller, 0));
    NvmeWrite64(Controller, NVME_REG_ACQ, NvmeDmaAddress(Controller, 1));
    FailureStage = "controller enable";
    NvmeWrite32(Controller, NVME_REG_CC, NVME_CC_EN | NVME_SQES_64 | NVME_CQES_16);
    if (!NvmeWaitReady(Controller, true))
        goto Failure;

    FailureStage = "identify controller";
    if (!HZ_SUCCESS(NvmeIdentify(Controller, 0, NVME_IDENTIFY_CONTROLLER)))
        goto Failure;
    uint8_t *Identify = Controller->Dma + 4 * NVME_PAGE_SIZE;
    for (uint32_t i = 0; i < sizeof(Controller->Model) - 1; i++)
        Controller->Model[i] = (char)Identify[24 + i];
    Controller->Model[sizeof(Controller->Model) - 1] = '\0';
    for (int i = (int)sizeof(Controller->Model) - 2; i >= 0 &&
             (Controller->Model[i] == ' ' || Controller->Model[i] == '\0'); i--)
        Controller->Model[i] = '\0';
    if (!Controller->Model[0])
        RtlStringCopy(Controller->Model, sizeof(Controller->Model), "NVMe namespace");
    uint32_t MaxDataBytes = NVME_BOUNCE_BYTES;
    uint8_t Mdts = Identify[77];
    if (Mdts && Mdts < 4)
        MaxDataBytes = NVME_PAGE_SIZE << Mdts;

    memset(Identify, 0, NVME_PAGE_SIZE);
    FailureStage = "identify active namespaces";
    if (!HZ_SUCCESS(NvmeIdentify(Controller, 0, NVME_IDENTIFY_ACTIVE_NS_LIST)))
        goto Failure;
    uint32_t NamespaceId = *(uint32_t *)Identify;
    if (!NamespaceId)
        goto Failure;
    memset(Identify, 0, NVME_PAGE_SIZE);
    FailureStage = "identify namespace";
    if (!HZ_SUCCESS(NvmeIdentify(Controller, NamespaceId, NVME_IDENTIFY_NAMESPACE)))
        goto Failure;
    Controller->NamespaceId = NamespaceId;
    Controller->SectorCount = *(uint64_t *)(Identify + 0);
    uint8_t FormatIndex = Identify[26] & 0x0Fu;
    uint8_t *LbaFormat = Identify + 128 + (uint32_t)FormatIndex * 4u;
    uint16_t MetadataBytes = *(uint16_t *)LbaFormat;
    uint8_t LbaDataShift = LbaFormat[2];
    if (!Controller->SectorCount || MetadataBytes || LbaDataShift < 9 || LbaDataShift > 12)
        goto Failure;
    Controller->SectorSize = 1u << LbaDataShift;
    Controller->MaxTransferSectors = MaxDataBytes / Controller->SectorSize;
    if (!Controller->MaxTransferSectors)
        goto Failure;
    if (Controller->MaxTransferSectors > NVME_BOUNCE_BYTES / Controller->SectorSize)
        Controller->MaxTransferSectors = NVME_BOUNCE_BYTES / Controller->SectorSize;

    NvmeInitializeQueue(Controller, &Controller->Io, 1, 2, 3);
    memset(Identify, 0, NVME_PAGE_SIZE);
    NVME_COMMAND CreateQueue;
    memset(&CreateQueue, 0, sizeof(CreateQueue));
    FailureStage = "create I/O completion queue";
    CreateQueue.Opcode = NVME_ADMIN_CREATE_IO_CQ;
    CreateQueue.Prp1 = NvmeDmaAddress(Controller, 3);
    CreateQueue.CommandDword10 = 1u | ((NVME_QUEUE_DEPTH - 1u) << 16);
    CreateQueue.CommandDword11 = 1u; /* physically contiguous, interrupts disabled */
    if (!HZ_SUCCESS(NvmeSubmit(&Controller->Admin, &CreateQueue, NULL)))
        goto Failure;
    memset(&CreateQueue, 0, sizeof(CreateQueue));
    FailureStage = "create I/O submission queue";
    CreateQueue.Opcode = NVME_ADMIN_CREATE_IO_SQ;
    CreateQueue.Prp1 = NvmeDmaAddress(Controller, 2);
    CreateQueue.CommandDword10 = 1u | ((NVME_QUEUE_DEPTH - 1u) << 16);
    CreateQueue.CommandDword11 = 1u | (1u << 16); /* contiguous, completion queue 1 */
    if (!HZ_SUCCESS(NvmeSubmit(&Controller->Admin, &CreateQueue, NULL)))
        goto Failure;

    DISK_DESCRIPTION Disk;
    memset(&Disk, 0, sizeof(Disk));
    RtlStringCopy(Disk.Model, sizeof(Disk.Model), Controller->Model);
    Disk.SectorSize = Controller->SectorSize;
    Disk.SectorCount = Controller->SectorCount;
    Disk.MaxTransferSectors = Controller->MaxTransferSectors;
    Disk.Transfer = NvmeTransfer;
    Disk.Context = Controller;
    uint32_t DiskNumber;
    FailureStage = "register namespace as a disk";
    if (!HZ_SUCCESS(DiskCreateDisk(&Disk, &DiskNumber)))
        goto Failure;

    DbgPrint("NVMe: %02x:%02x.%u namespace %u, %s, %llu sectors of %u bytes (PCI %04x:%04x, NVMe %u.%u.%u)\n",
             Pci->Bus, Pci->Device, Pci->Function, NamespaceId, Controller->Model,
             (unsigned long long)Controller->SectorCount, Controller->SectorSize,
             Pci->VendorId, Pci->DeviceId, (Version >> 16) & 0xFFFFu, (Version >> 8) & 0xFFu, Version & 0xFFu);
    return true;

Failure:
    DbgPrintEx(DPFLTR_WARNING_LEVEL, "NVMe: %02x:%02x.%u initialization failed at %s (CC 0x%08x, CSTS 0x%08x)\n",
               Pci->Bus, Pci->Device, Pci->Function, FailureStage,
               NvmeRead32(Controller, NVME_REG_CC), NvmeRead32(Controller, NVME_REG_CSTS));
    NvmeWrite32(Controller, NVME_REG_CC, 0);
    if (NvmeWaitReady(Controller, false))
        MmFreeContiguousPages(Controller->DmaPfn, NVME_DMA_PAGES);
    ExFreePoolWithTag(Controller, NVME_TAG);
    return false;
}

static STATUS NvmeDriverEntry(DRIVER_OBJECT *Driver)
{
    (void)Driver;
    uint32_t Started = 0;
    for (PCI_DEVICE *Pci = PciFindDevice(0x01, 0x08, 0x02, NULL); Pci;
         Pci = PciFindDevice(0x01, 0x08, 0x02, Pci)) {
        if (NvmeInitializeController(Pci)) {
            PciSetDriverStatus(Pci, PciDriverStarted, "Nexora NVMe storage");
            Started++;
        } else {
            DbgPrintEx(DPFLTR_WARNING_LEVEL, "NVMe: %02x:%02x.%u %04x:%04x was detected but could not start\n",
                       Pci->Bus, Pci->Device, Pci->Function, Pci->VendorId, Pci->DeviceId);
            PciSetDriverStatus(Pci, PciDriverStartFailed, "Nexora NVMe storage");
        }
    }
    return Started ? STATUS_SUCCESS : STATUS_NO_SUCH_DEVICE;
}

IO_DECLARE_BUILTIN_DRIVER(NvmeBuiltinDriver, "\\Driver\\Nvme", NvmeDriverEntry, IO_ORDER_DEVICE);
