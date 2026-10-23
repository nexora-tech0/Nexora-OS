/*
 * Nexora OS - drivers/ahci/ahci.c
 * AHCI (SATA) port driver.
 *
 * Every SATA controller (PCI class 01.06.01) is taken over from the firmware: ports are stopped
 * and reprogrammed with our own command list, received-FIS area and command table, one command
 * slot per port. Each ATA disk is identified and handed to the disk class driver (DiskCreateDisk).
 * ATAPI devices (the boot DVD) are left alone.
 *
 * Transfers go through a 64 KiB physically contiguous bounce buffer per port (below 4 GiB), so
 * one PRD entry describes every command and callers may pass any kernel buffer. Commands are
 * serialized per port by a mutex. Completion is signaled by MSI (ISR -> DPC -> event); without
 * MSI the waiting thread polls the command-issue register every millisecond.
 */
#include "disk.h"
#include "ex.h"
#include "hal.h"
#include "io.h"
#include "mm.h"
#include "pci.h"
#include "rtl.h"

#define AHCI_TAG HZ_POOL_TAG('A', 'h', 'c', 'i')
#define AHCI_IRQL 4
#define AHCI_BOUNCE_PAGES 16
#define AHCI_BOUNCE_SIZE (AHCI_BOUNCE_PAGES * PAGE_SIZE)
#define AHCI_COMMAND_TIMEOUT_US 5000000

/* HBA registers */
#define HBA_CAP 0x00
#define HBA_GHC 0x04
#define HBA_IS 0x08
#define HBA_PI 0x0C
#define HBA_VS 0x10
#define HBA_CAP2 0x24
#define HBA_BOHC 0x28
#define HBA_CAP_S64A (1u << 31)
#define HBA_CAP_SSS (1u << 27)
#define HBA_GHC_IE (1u << 1)
#define HBA_GHC_AE (1u << 31)

/* Port registers */
#define PX_CLB 0x00
#define PX_CLBU 0x04
#define PX_FB 0x08
#define PX_FBU 0x0C
#define PX_IS 0x10
#define PX_IE 0x14
#define PX_CMD 0x18
#define PX_TFD 0x20
#define PX_SIG 0x24
#define PX_SSTS 0x28
#define PX_SERR 0x30
#define PX_CI 0x38
#define PX_CMD_ST (1u << 0)
#define PX_CMD_SUD (1u << 1)
#define PX_CMD_FRE (1u << 4)
#define PX_CMD_FR (1u << 14)
#define PX_CMD_CR (1u << 15)
#define PX_IS_TFES (1u << 30)
#define PX_IS_ERRORS (PX_IS_TFES | (1u << 29) | (1u << 28) | (1u << 27))
#define PX_IE_DEFAULT (0x0000000Fu | PX_IS_ERRORS)
#define PX_TFD_ERR 0x01
#define PX_TFD_DRQ 0x08
#define PX_TFD_BSY 0x80

#define SATA_SIG_ATA 0x00000101u
#define SATA_SIG_ATAPI 0xEB140101u

#define ATA_CMD_READ_DMA_EXT 0x25
#define ATA_CMD_WRITE_DMA_EXT 0x35
#define ATA_CMD_FLUSH_CACHE_EXT 0xEA
#define ATA_CMD_IDENTIFY 0xEC

typedef struct _AHCI_CONTROLLER AHCI_CONTROLLER;

typedef struct _AHCI_PORT {
    AHCI_CONTROLLER *Hba;
    uint32_t Number;
    volatile uint8_t *Regs;
    uint8_t *Memory;             /* command list (1 KiB) | received FIS (256 B) | command table */
    uint64_t MemoryPhysical;
    uint8_t *Bounce;
    uint64_t BouncePhysical;
    KMUTEX Lock;
    KEVENT Done;
    volatile uint32_t PendingStatus;
    uint64_t Sectors;
    uint32_t SectorSize;
    char Model[41];
    uint64_t Commands;
} AHCI_PORT;

struct _AHCI_CONTROLLER {
    PCI_DEVICE *Pci;
    volatile uint8_t *Abar;
    uint32_t Cap;
    bool Msi;
    uint8_t Vector;
    KDPC Dpc;
    AHCI_PORT *Ports[32];
};

static uint32_t AhciRead(volatile uint8_t *Base, uint32_t Offset)
{
    return *(volatile uint32_t *)(Base + Offset);
}

static void AhciWrite(volatile uint8_t *Base, uint32_t Offset, uint32_t Value)
{
    *(volatile uint32_t *)(Base + Offset) = Value;
}

static bool AhciWaitClear(volatile uint8_t *Base, uint32_t Offset, uint32_t Mask, uint32_t TimeoutMs)
{
    for (uint32_t i = 0; i <= TimeoutMs * 10; i++) {
        if (!(AhciRead(Base, Offset) & Mask))
            return true;
        KeStallExecutionProcessor(100);
    }
    return false;
}

/* ---- Interrupts ------------------------------------------------------------------------------- */

static void AhciDpc(KDPC *Dpc, void *Context, void *Arg1, void *Arg2)
{
    AHCI_CONTROLLER *Hba = Context;
    for (int i = 0; i < 32; i++) {
        if (Hba->Ports[i] && Hba->Ports[i]->PendingStatus)
            KeSetEvent(&Hba->Ports[i]->Done);
    }
}

static bool AhciInterrupt(KTRAP_FRAME *Frame, void *Context)
{
    AHCI_CONTROLLER *Hba = Context;
    uint32_t Pending = AhciRead(Hba->Abar, HBA_IS);
    if (!Pending)
        return false;
    for (int i = 0; i < 32; i++) {
        if (!(Pending & (1u << i)))
            continue;
        volatile uint8_t *Port = Hba->Abar + 0x100 + i * 0x80;
        uint32_t Status = AhciRead(Port, PX_IS);
        AhciWrite(Port, PX_IS, Status);
        if (Hba->Ports[i])
            __atomic_or_fetch(&Hba->Ports[i]->PendingStatus, Status | 0x80000000u, __ATOMIC_RELAXED);
    }
    AhciWrite(Hba->Abar, HBA_IS, Pending);
    KeInsertQueueDpc(&Hba->Dpc, NULL, NULL);
    return true;
}

/* ---- Port control ------------------------------------------------------------------------------ */

static bool AhciStopPort(volatile uint8_t *Port)
{
    AhciWrite(Port, PX_CMD, AhciRead(Port, PX_CMD) & ~PX_CMD_ST);
    if (!AhciWaitClear(Port, PX_CMD, PX_CMD_CR, 500))
        return false;
    AhciWrite(Port, PX_CMD, AhciRead(Port, PX_CMD) & ~PX_CMD_FRE);
    return AhciWaitClear(Port, PX_CMD, PX_CMD_FR, 500);
}

static void AhciStartPort(volatile uint8_t *Port)
{
    AhciWaitClear(Port, PX_TFD, PX_TFD_BSY | PX_TFD_DRQ, 1000);
    AhciWrite(Port, PX_CMD, AhciRead(Port, PX_CMD) | PX_CMD_FRE);
    AhciWrite(Port, PX_CMD, AhciRead(Port, PX_CMD) | PX_CMD_ST);
}

static void AhciRecoverPort(AHCI_PORT *P)
{
    AhciStopPort(P->Regs);
    AhciWrite(P->Regs, PX_SERR, 0xFFFFFFFF);
    AhciWrite(P->Regs, PX_IS, 0xFFFFFFFF);
    AhciStartPort(P->Regs);
}

/* Issues one command in slot 0 and waits for it. Bytes go through the bounce buffer. */
static STATUS AhciExecute(AHCI_PORT *P, uint8_t Command, uint64_t Lba, uint32_t Count, uint32_t Bytes, bool Write)
{
    uint8_t *Header = P->Memory;
    uint8_t *Table = P->Memory + 2048;
    memset(Header, 0, 32);
    memset(Table, 0, 0x90);
    uint32_t Dw0 = 5 /* CFL: 5 dwords */ | (Write ? (1u << 6) : 0) | ((Bytes ? 1u : 0u) << 16);
    uint64_t TablePhysical = P->MemoryPhysical + 2048;
    memcpy(Header + 0, &Dw0, 4);
    uint32_t Lo = (uint32_t)TablePhysical, Hi = (uint32_t)(TablePhysical >> 32);
    memcpy(Header + 8, &Lo, 4);
    memcpy(Header + 12, &Hi, 4);

    uint8_t *Fis = Table;
    Fis[0] = 0x27; /* register host-to-device */
    Fis[1] = 0x80; /* command */
    Fis[2] = Command;
    Fis[4] = (uint8_t)Lba;
    Fis[5] = (uint8_t)(Lba >> 8);
    Fis[6] = (uint8_t)(Lba >> 16);
    Fis[7] = 0x40; /* LBA mode */
    Fis[8] = (uint8_t)(Lba >> 24);
    Fis[9] = (uint8_t)(Lba >> 32);
    Fis[10] = (uint8_t)(Lba >> 40);
    Fis[12] = (uint8_t)Count;
    Fis[13] = (uint8_t)(Count >> 8);

    if (Bytes) {
        uint8_t *Prd = Table + 0x80;
        uint32_t BLo = (uint32_t)P->BouncePhysical, BHi = (uint32_t)(P->BouncePhysical >> 32);
        uint32_t Dbc = (Bytes - 1) | (1u << 31);
        memcpy(Prd + 0, &BLo, 4);
        memcpy(Prd + 4, &BHi, 4);
        memcpy(Prd + 12, &Dbc, 4);
    }

    if (!AhciWaitClear(P->Regs, PX_TFD, PX_TFD_BSY | PX_TFD_DRQ, 1000))
        AhciRecoverPort(P);
    KeResetEvent(&P->Done);
    P->PendingStatus = 0;
    AhciWrite(P->Regs, PX_IS, 0xFFFFFFFF);
    __asm__ volatile("" : : : "memory");
    AhciWrite(P->Regs, PX_CI, 1);
    P->Commands++;

    /* Without MSI nothing wakes the waiter early, and clock ticks can be late under a hypervisor:
       spin on the command-issue register for up to 2 ms first (most commands finish sooner). */
    if (!P->Hba->Msi) {
        for (int i = 0; i < 200 && (AhciRead(P->Regs, PX_CI) & 1) && !(AhciRead(P->Regs, PX_IS) & PX_IS_ERRORS); i++)
            KeStallExecutionProcessor(10);
    }
    uint64_t Start = KeQueryUptimeMicroseconds();
    STATUS Status = STATUS_IO_TIMEOUT;
    for (;;) {
        uint32_t Is = AhciRead(P->Regs, PX_IS) | P->PendingStatus;
        if (Is & PX_IS_ERRORS) {
            Status = STATUS_DEVICE_DATA_ERROR;
            break;
        }
        if (!(AhciRead(P->Regs, PX_CI) & 1)) {
            Status = (AhciRead(P->Regs, PX_TFD) & PX_TFD_ERR) ? STATUS_DEVICE_DATA_ERROR : STATUS_SUCCESS;
            break;
        }
        if (KeQueryUptimeMicroseconds() - Start > AHCI_COMMAND_TIMEOUT_US)
            break;
        uint64_t Poll = 1000;
        KeWaitForSingleObject(&P->Done, &Poll);
    }
    if (!HZ_SUCCESS(Status)) {
        DbgPrintEx(DPFLTR_ERROR_LEVEL, "AHCI: port %u command 0x%02X at LBA %llu failed (TFD 0x%x, IS 0x%x): 0x%08X\n",
                   P->Number, Command, (unsigned long long)Lba, AhciRead(P->Regs, PX_TFD),
                   AhciRead(P->Regs, PX_IS) | P->PendingStatus, (uint32_t)Status);
        AhciRecoverPort(P);
    }
    return Status;
}

static STATUS AhciTransfer(void *Context, uint64_t Lba, uint32_t Count, void *Buffer, bool Write)
{
    AHCI_PORT *P = Context;
    uint32_t Bytes = Count * P->SectorSize;
    if (Count == 0 || Bytes > AHCI_BOUNCE_SIZE || Lba + Count > P->Sectors)
        return STATUS_INVALID_PARAMETER;
    KeWaitForSingleObject(&P->Lock, NULL);
    if (Write)
        memcpy(P->Bounce, Buffer, Bytes);
    STATUS Status = AhciExecute(P, Write ? ATA_CMD_WRITE_DMA_EXT : ATA_CMD_READ_DMA_EXT, Lba, Count, Bytes, Write);
    if (HZ_SUCCESS(Status) && !Write)
        memcpy(Buffer, P->Bounce, Bytes);
    KeReleaseMutex(&P->Lock);
    return Status;
}

static void AhciCopyAtaString(char *Out, const uint16_t *Words, unsigned FirstWord, unsigned WordCount)
{
    unsigned n = 0;
    for (unsigned i = 0; i < WordCount; i++) {
        uint16_t W = Words[FirstWord + i];
        Out[n++] = (char)(W >> 8);
        Out[n++] = (char)(W & 0xFF);
    }
    Out[n] = '\0';
    while (n > 0 && (Out[n - 1] == ' ' || Out[n - 1] == '\0'))
        Out[--n] = '\0';
    for (unsigned i = 0; i < n; i++) {
        if ((uint8_t)Out[i] < 0x20 || (uint8_t)Out[i] > 0x7E)
            Out[i] = '?';
    }
}

static void AhciInitializePort(AHCI_CONTROLLER *Hba, uint32_t Number)
{
    volatile uint8_t *Regs = Hba->Abar + 0x100 + Number * 0x80;
    uint32_t Ssts = AhciRead(Regs, PX_SSTS);
    if ((Ssts & 0xF) != 3) {
        AhciWrite(Regs, PX_IE, 0);
        return; /* no device, or link not up */
    }
    uint32_t Signature = AhciRead(Regs, PX_SIG);
    if (Signature != SATA_SIG_ATA) {
        DbgPrintEx(DPFLTR_TRACE_LEVEL, "AHCI: port %u: %s device (signature 0x%08x) left alone\n", Number,
                   Signature == SATA_SIG_ATAPI ? "ATAPI" : "non-ATA", Signature);
        return;
    }

    AHCI_PORT *P = ExAllocatePoolZero(NonPagedPool, sizeof(AHCI_PORT), AHCI_TAG);
    PFN_NUMBER MemoryPfn, BouncePfn;
    if (!P || !HZ_SUCCESS(MmAllocateContiguousPages(1, 1ull << 32, 1, &MemoryPfn))) {
        if (P)
            ExFreePoolWithTag(P, AHCI_TAG);
        return;
    }
    if (!HZ_SUCCESS(MmAllocateContiguousPages(AHCI_BOUNCE_PAGES, 1ull << 32, 1, &BouncePfn))) {
        MmFreeContiguousPages(MemoryPfn, 1);
        ExFreePoolWithTag(P, AHCI_TAG);
        return;
    }
    P->Hba = Hba;
    P->Number = Number;
    P->Regs = Regs;
    P->MemoryPhysical = MemoryPfn << PAGE_SHIFT;
    P->Memory = MmPfnToVirtual(MemoryPfn);
    P->BouncePhysical = BouncePfn << PAGE_SHIFT;
    P->Bounce = MmPfnToVirtual(BouncePfn);
    memset(P->Memory, 0, PAGE_SIZE);
    KeInitializeMutex(&P->Lock);
    KeInitializeEvent(&P->Done, NotificationEvent, false);

    if (!AhciStopPort(Regs)) {
        DbgPrintEx(DPFLTR_WARNING_LEVEL, "AHCI: port %u does not stop; skipped\n", Number);
        return; /* the firmware still owns it; leak the small allocation rather than race the DMA */
    }
    AhciWrite(Regs, PX_CLB, (uint32_t)P->MemoryPhysical);
    AhciWrite(Regs, PX_CLBU, (uint32_t)(P->MemoryPhysical >> 32));
    AhciWrite(Regs, PX_FB, (uint32_t)(P->MemoryPhysical + 1024));
    AhciWrite(Regs, PX_FBU, (uint32_t)((P->MemoryPhysical + 1024) >> 32));
    AhciWrite(Regs, PX_SERR, 0xFFFFFFFF);
    AhciWrite(Regs, PX_IS, 0xFFFFFFFF);
    AhciWrite(Regs, PX_IE, Hba->Msi ? PX_IE_DEFAULT : 0);
    if (Hba->Cap & HBA_CAP_SSS)
        AhciWrite(Regs, PX_CMD, AhciRead(Regs, PX_CMD) | PX_CMD_SUD);
    Hba->Ports[Number] = P;
    AhciStartPort(Regs);

    /* IDENTIFY DEVICE */
    P->SectorSize = 512;
    P->Sectors = 1; /* lets the bounds check accept IDENTIFY's LBA 0 */
    KeWaitForSingleObject(&P->Lock, NULL);
    STATUS Status = AhciExecute(P, ATA_CMD_IDENTIFY, 0, 0, 512, false);
    uint16_t Identify[256];
    if (HZ_SUCCESS(Status))
        memcpy(Identify, P->Bounce, sizeof(Identify));
    KeReleaseMutex(&P->Lock);
    if (!HZ_SUCCESS(Status)) {
        DbgPrintEx(DPFLTR_WARNING_LEVEL, "AHCI: port %u: IDENTIFY failed (0x%08X)\n", Number, (uint32_t)Status);
        return;
    }
    AhciCopyAtaString(P->Model, Identify, 27, 20);
    bool Lba48 = (Identify[83] & (1u << 10)) != 0;
    P->Sectors = Lba48 ? ((uint64_t)Identify[100] | ((uint64_t)Identify[101] << 16) | ((uint64_t)Identify[102] << 32) |
                          ((uint64_t)Identify[103] << 48))
                       : ((uint64_t)Identify[60] | ((uint64_t)Identify[61] << 16));
    if ((Identify[106] & 0xC000) == 0x4000 && (Identify[106] & (1u << 12)))
        P->SectorSize = 2u * ((uint32_t)Identify[117] | ((uint32_t)Identify[118] << 16));
    if (!Lba48 || P->SectorSize != 512) {
        DbgPrintEx(DPFLTR_WARNING_LEVEL, "AHCI: port %u: '%s' needs %s; skipped\n", Number, P->Model,
                   !Lba48 ? "LBA28 commands" : "non-512-byte sectors");
        return;
    }

    DISK_DESCRIPTION Disk;
    memset(&Disk, 0, sizeof(Disk));
    RtlStringCopy(Disk.Model, sizeof(Disk.Model), P->Model);
    Disk.SectorSize = P->SectorSize;
    Disk.SectorCount = P->Sectors;
    Disk.MaxTransferSectors = AHCI_BOUNCE_SIZE / P->SectorSize;
    Disk.Transfer = AhciTransfer;
    Disk.Context = P;
    uint32_t DiskNumber = 0;
    Status = DiskCreateDisk(&Disk, &DiskNumber);
    DbgPrintEx(HZ_SUCCESS(Status) ? DPFLTR_TRACE_LEVEL : DPFLTR_WARNING_LEVEL,
               "AHCI: port %u: '%s', %llu sectors -> Harddisk%u (0x%08X)\n", Number, P->Model,
               (unsigned long long)P->Sectors, DiskNumber, (uint32_t)Status);
}

static void AhciInitializeController(PCI_DEVICE *Pci)
{
    if (!Pci->Bar[5] || Pci->BarIsIo[5])
        return;
    AHCI_CONTROLLER *Hba = ExAllocatePoolZero(NonPagedPool, sizeof(AHCI_CONTROLLER), AHCI_TAG);
    if (!Hba)
        return;
    Hba->Pci = Pci;
    uint64_t Size = HZ_MAX(Pci->BarSize[5], 0x1100);
    Hba->Abar = MmMapIoSpace(Pci->Bar[5], Size, MM_CACHE_UNCACHED);
    if (!Hba->Abar) {
        ExFreePoolWithTag(Hba, AHCI_TAG);
        return;
    }
    PciEnableDevice(Pci, PCI_COMMAND_MEMORY | PCI_COMMAND_BUS_MASTER);

    /* BIOS/OS handoff when the controller supports it. */
    if (AhciRead(Hba->Abar, HBA_CAP2) & 1) {
        AhciWrite(Hba->Abar, HBA_BOHC, AhciRead(Hba->Abar, HBA_BOHC) | (1u << 1));
        AhciWaitClear(Hba->Abar, HBA_BOHC, 1u << 0, 100);
    }
    AhciWrite(Hba->Abar, HBA_GHC, AhciRead(Hba->Abar, HBA_GHC) | HBA_GHC_AE);
    Hba->Cap = AhciRead(Hba->Abar, HBA_CAP);
    uint32_t Implemented = AhciRead(Hba->Abar, HBA_PI);
    uint32_t Version = AhciRead(Hba->Abar, HBA_VS);

    KeInitializeDpc(&Hba->Dpc, AhciDpc, Hba);
    if (Pci->MsiCapability && HZ_SUCCESS(KeConnectInterruptAnyVector(AHCI_IRQL, AhciInterrupt, Hba, &Hba->Vector))) {
        if (HZ_SUCCESS(PciEnableMsi(Pci, Hba->Vector)))
            Hba->Msi = true;
        else
            KeDisconnectInterrupt(Hba->Vector);
    }
    AhciWrite(Hba->Abar, HBA_IS, 0xFFFFFFFF);
    if (Hba->Msi)
        AhciWrite(Hba->Abar, HBA_GHC, AhciRead(Hba->Abar, HBA_GHC) | HBA_GHC_IE);

    DbgPrint("AHCI: %02x:%02x.%u %04x:%04x, AHCI %u.%u, %u ports (implemented 0x%x), %s\n", Pci->Bus, Pci->Device,
             Pci->Function, Pci->VendorId, Pci->DeviceId, Version >> 16, (Version >> 8) & 0xFF, (Hba->Cap & 0x1F) + 1,
             Implemented, Hba->Msi ? "MSI" : "polled completion");
    for (uint32_t i = 0; i < 32; i++) {
        if (Implemented & (1u << i))
            AhciInitializePort(Hba, i);
    }
}

static STATUS AhciDriverEntry(DRIVER_OBJECT *Driver)
{
    uint32_t Count = 0;
    for (PCI_DEVICE *Pci = PciFindDevice(0x01, 0x06, 0x01, NULL); Pci; Pci = PciFindDevice(0x01, 0x06, 0x01, Pci)) {
        AhciInitializeController(Pci);
        Count++;
    }
    return Count ? STATUS_SUCCESS : STATUS_NO_SUCH_DEVICE;
}

IO_DECLARE_BUILTIN_DRIVER(AhciBuiltinDriver, "\\Driver\\Ahci", AhciDriverEntry, IO_ORDER_DEVICE);
