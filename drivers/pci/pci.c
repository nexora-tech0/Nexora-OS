/*
 * Nexora OS - drivers/pci/pci.c
 * PCI bus driver: configuration access through ECAM (the ACPI MCFG table; one 1 MiB window per
 * bus, mapped uncached on first use) or the legacy 0xCF8/0xCFC ports, a recursive scan from bus 0
 * through PCI-to-PCI bridges, BAR sizing and MSI programming.
 */
#include "acpi.h"
#include "ex.h"
#include "hal.h"
#include "io.h"
#include "mm.h"
#include "pci.h"
#include "rtl.h"

#define PCI_TAG HZ_POOL_TAG('P', 'c', 'i', ' ')
#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA 0xCFC
#define PCI_CAP_MSI 0x05
#define PCI_CAP_MSIX 0x11
#define PCI_STATUS_CAPABILITIES 0x0010

typedef struct HZ_PACKED _ACPI_MCFG_ENTRY {
    uint64_t BaseAddress;
    uint16_t Segment;
    uint8_t StartBus;
    uint8_t EndBus;
    uint32_t Reserved;
} ACPI_MCFG_ENTRY;

static uint64_t PcipEcamBase;
static uint8_t PcipEcamStartBus, PcipEcamEndBus;
static volatile uint8_t *PcipEcamBus[256];
static KSPIN_LOCK PcipPortLock;
static PCI_DEVICE *PcipDevices;
static uint32_t PcipDeviceCount, PcipBusCount;

static volatile uint8_t *PcipEcam(uint8_t Bus, uint8_t Device, uint8_t Function, uint32_t Offset)
{
    if (!PcipEcamBase || Bus < PcipEcamStartBus || Bus > PcipEcamEndBus)
        return NULL;
    if (!PcipEcamBus[Bus]) {
        uint64_t Physical = PcipEcamBase + ((uint64_t)(Bus - PcipEcamStartBus) << 20);
        PcipEcamBus[Bus] = MmMapIoSpace(Physical, 1u << 20, MM_CACHE_UNCACHED);
        if (!PcipEcamBus[Bus])
            return NULL;
    }
    return PcipEcamBus[Bus] + ((uint32_t)Device << 15) + ((uint32_t)Function << 12) + Offset;
}

static uint32_t PcipRead32(uint8_t Bus, uint8_t Device, uint8_t Function, uint32_t Offset)
{
    volatile uint8_t *Ecam = PcipEcam(Bus, Device, Function, Offset & ~3u);
    if (Ecam)
        return *(volatile uint32_t *)Ecam;
    if (Offset >= 256)
        return 0xFFFFFFFF;
    KIRQL OldIrql;
    KeAcquireSpinLockRaiseToLevel(&PcipPortLock, HIGH_LEVEL, &OldIrql);
    HalWritePort32(PCI_CONFIG_ADDRESS, 0x80000000u | ((uint32_t)Bus << 16) | ((uint32_t)Device << 11) |
                                           ((uint32_t)Function << 8) | (Offset & 0xFC));
    uint32_t Value = HalReadPort32(PCI_CONFIG_DATA);
    KeReleaseSpinLock(&PcipPortLock, OldIrql);
    return Value;
}

static void PcipWrite32(uint8_t Bus, uint8_t Device, uint8_t Function, uint32_t Offset, uint32_t Value)
{
    volatile uint8_t *Ecam = PcipEcam(Bus, Device, Function, Offset & ~3u);
    if (Ecam) {
        *(volatile uint32_t *)Ecam = Value;
        return;
    }
    if (Offset >= 256)
        return;
    KIRQL OldIrql;
    KeAcquireSpinLockRaiseToLevel(&PcipPortLock, HIGH_LEVEL, &OldIrql);
    HalWritePort32(PCI_CONFIG_ADDRESS, 0x80000000u | ((uint32_t)Bus << 16) | ((uint32_t)Device << 11) |
                                           ((uint32_t)Function << 8) | (Offset & 0xFC));
    HalWritePort32(PCI_CONFIG_DATA, Value);
    KeReleaseSpinLock(&PcipPortLock, OldIrql);
}

uint32_t PciReadConfig32(const PCI_DEVICE *D, uint32_t Offset)
{
    return PcipRead32(D->Bus, D->Device, D->Function, Offset);
}

uint16_t PciReadConfig16(const PCI_DEVICE *D, uint32_t Offset)
{
    return (uint16_t)(PciReadConfig32(D, Offset & ~3u) >> ((Offset & 2) * 8));
}

uint8_t PciReadConfig8(const PCI_DEVICE *D, uint32_t Offset)
{
    return (uint8_t)(PciReadConfig32(D, Offset & ~3u) >> ((Offset & 3) * 8));
}

void PciWriteConfig32(const PCI_DEVICE *D, uint32_t Offset, uint32_t Value)
{
    PcipWrite32(D->Bus, D->Device, D->Function, Offset, Value);
}

void PciWriteConfig16(const PCI_DEVICE *D, uint32_t Offset, uint16_t Value)
{
    uint32_t Shift = (Offset & 2) * 8;
    uint32_t Old = PciReadConfig32(D, Offset & ~3u);
    PciWriteConfig32(D, Offset & ~3u, (Old & ~(0xFFFFu << Shift)) | ((uint32_t)Value << Shift));
}

PCI_DEVICE *PciGetDeviceList(void)
{
    return PcipDevices;
}

PCI_DEVICE *PciFindDevice(uint8_t Class, uint8_t Subclass, uint8_t ProgIf, PCI_DEVICE *Previous)
{
    for (PCI_DEVICE *D = Previous ? Previous->Next : PcipDevices; D; D = D->Next) {
        if (D->Class == Class && D->Subclass == Subclass && (ProgIf == 0xFF || D->ProgIf == ProgIf))
            return D;
    }
    return NULL;
}

void PciEnableDevice(PCI_DEVICE *Device, uint16_t CommandBits)
{
    PciWriteConfig16(Device, 0x04, PciReadConfig16(Device, 0x04) | CommandBits);
}

STATUS PciEnableMsi(PCI_DEVICE *Device, uint8_t Vector)
{
    uint8_t Cap = Device->MsiCapability;
    if (!Cap)
        return STATUS_NOT_SUPPORTED;
    uint16_t Control = PciReadConfig16(Device, Cap + 2);
    bool Is64Bit = (Control & 0x0080) != 0;
    PciWriteConfig32(Device, Cap + 4, 0xFEE00000u | ((HalGetLocalApicId() & 0xFF) << 12));
    if (Is64Bit) {
        PciWriteConfig32(Device, Cap + 8, 0);
        PciWriteConfig16(Device, Cap + 12, Vector); /* edge, fixed delivery */
    } else {
        PciWriteConfig16(Device, Cap + 8, Vector);
    }
    Control &= (uint16_t)~0x0070; /* one message */
    PciWriteConfig16(Device, Cap + 2, Control | 0x0001);
    PciEnableDevice(Device, PCI_COMMAND_INTX_DISABLE);
    return STATUS_SUCCESS;
}

/* ---- Enumeration -------------------------------------------------------------------------------- */

static void PcipSizeBars(PCI_DEVICE *D)
{
    unsigned Count = (D->HeaderType & 0x7F) == 0 ? 6 : (D->HeaderType & 0x7F) == 1 ? 2 : 0;
    uint16_t Command = PciReadConfig16(D, 0x04);
    PciWriteConfig16(D, 0x04, Command & ~(PCI_COMMAND_IO | PCI_COMMAND_MEMORY)); /* no decoding while sizing */
    for (unsigned i = 0; i < Count; i++) {
        uint32_t Offset = 0x10 + i * 4;
        uint32_t Original = PciReadConfig32(D, Offset);
        PciWriteConfig32(D, Offset, 0xFFFFFFFF);
        uint32_t Mask = PciReadConfig32(D, Offset);
        PciWriteConfig32(D, Offset, Original);
        if (Mask == 0 || Mask == 0xFFFFFFFF)
            continue;
        if (Original & 1) {
            D->BarIsIo[i] = true;
            D->Bar[i] = Original & ~3u;
            D->BarSize[i] = (uint16_t)(~(Mask & ~3u) + 1);
            continue;
        }
        uint64_t Base = Original & ~0xFu;
        uint64_t SizeMask = Mask & ~0xFu;
        if (((Original >> 1) & 3) == 2 && i + 1 < Count) { /* 64-bit BAR: the next one is the high half */
            uint32_t High = PciReadConfig32(D, Offset + 4);
            PciWriteConfig32(D, Offset + 4, 0xFFFFFFFF);
            uint32_t HighMask = PciReadConfig32(D, Offset + 4);
            PciWriteConfig32(D, Offset + 4, High);
            Base |= (uint64_t)High << 32;
            SizeMask |= (uint64_t)HighMask << 32;
            D->Bar[i] = Base;
            D->BarSize[i] = ~SizeMask + 1;
            i++;
        } else {
            D->Bar[i] = Base;
            D->BarSize[i] = (uint32_t)(~(uint32_t)SizeMask + 1);
        }
    }
    PciWriteConfig16(D, 0x04, Command);
}

static void PcipFindCapabilities(PCI_DEVICE *D)
{
    if (!(PciReadConfig16(D, 0x06) & PCI_STATUS_CAPABILITIES))
        return;
    uint8_t Pointer = PciReadConfig8(D, (D->HeaderType & 0x7F) == 2 ? 0x14 : 0x34) & 0xFC;
    for (int Guard = 0; Pointer && Guard < 48; Guard++) {
        uint8_t Id = PciReadConfig8(D, Pointer);
        if (Id == PCI_CAP_MSI)
            D->MsiCapability = Pointer;
        else if (Id == PCI_CAP_MSIX)
            D->MsixCapability = Pointer;
        Pointer = PciReadConfig8(D, Pointer + 1) & 0xFC;
    }
}

static const char *PcipClassName(uint8_t Class, uint8_t Subclass)
{
    switch ((Class << 8) | Subclass) {
    case 0x0101: return "IDE controller";
    case 0x0106: return "SATA controller";
    case 0x0108: return "NVMe controller";
    case 0x0200: return "Ethernet controller";
    case 0x0300: return "VGA display";
    case 0x0380: return "display controller";
    case 0x0401: return "audio device";
    case 0x0403: return "HD audio";
    case 0x0600: return "host bridge";
    case 0x0601: return "ISA bridge";
    case 0x0604: return "PCI bridge";
    case 0x0680: return "bridge";
    case 0x0880: return "system peripheral";
    case 0x0C03: return "USB controller";
    case 0x0C05: return "SMBus controller";
    default: return "device";
    }
}

static void PcipScanBus(uint8_t Bus, int Depth)
{
    PcipBusCount++;
    for (uint8_t Device = 0; Device < 32; Device++) {
        for (uint8_t Function = 0; Function < 8; Function++) {
            uint32_t Id = PcipRead32(Bus, Device, Function, 0x00);
            if ((Id & 0xFFFF) == 0xFFFF) {
                if (Function == 0)
                    break;
                continue;
            }
            PCI_DEVICE *D = ExAllocatePoolZero(NonPagedPool, sizeof(PCI_DEVICE), PCI_TAG);
            if (!D)
                return;
            D->Bus = Bus;
            D->Device = Device;
            D->Function = Function;
            D->VendorId = (uint16_t)Id;
            D->DeviceId = (uint16_t)(Id >> 16);
            uint32_t ClassReg = PciReadConfig32(D, 0x08);
            D->Revision = (uint8_t)ClassReg;
            D->ProgIf = (uint8_t)(ClassReg >> 8);
            D->Subclass = (uint8_t)(ClassReg >> 16);
            D->Class = (uint8_t)(ClassReg >> 24);
            D->HeaderType = PciReadConfig8(D, 0x0E);
            uint32_t Interrupt = PciReadConfig32(D, 0x3C);
            D->InterruptLine = (uint8_t)Interrupt;
            D->InterruptPin = (uint8_t)(Interrupt >> 8);
            PcipSizeBars(D);
            PcipFindCapabilities(D);

            /* Append, keeping scan order. */
            PCI_DEVICE **Link = &PcipDevices;
            while (*Link)
                Link = &(*Link)->Next;
            *Link = D;
            PcipDeviceCount++;

            char Bars[256] = "";
            size_t Used = 0;
            for (int i = 0; i < 6 && Used < sizeof(Bars) - 48; i++) {
                if (D->Bar[i] || D->BarSize[i])
                    Used += (size_t)RtlFormatString(Bars + Used, sizeof(Bars) - Used, " BAR%d %s0x%llx/0x%llx", i,
                                                    D->BarIsIo[i] ? "io " : "", (unsigned long long)D->Bar[i],
                                                    (unsigned long long)D->BarSize[i]);
            }
            DbgPrintEx(DPFLTR_TRACE_LEVEL, "PCI: %02x:%02x.%u %04x:%04x class %02x.%02x.%02x %s%s%s%s\n", Bus, Device,
                       Function, D->VendorId, D->DeviceId, D->Class, D->Subclass, D->ProgIf,
                       PcipClassName(D->Class, D->Subclass), D->MsiCapability ? " MSI" : "",
                       D->MsixCapability ? " MSI-X" : "", Bars);

            if ((D->HeaderType & 0x7F) == 1 && Depth < 8) {
                uint8_t Secondary = PciReadConfig8(D, 0x19);
                if (Secondary > Bus)
                    PcipScanBus(Secondary, Depth + 1);
            }
            if (Function == 0 && !(D->HeaderType & 0x80))
                break; /* single-function device */
        }
    }
}

static STATUS PciDriverEntry(DRIVER_OBJECT *Driver)
{
    KeInitializeSpinLock(&PcipPortLock);
    const ACPI_TABLE_HEADER *Mcfg = HalAcpiFindTable("MCFG", 0);
    if (Mcfg && Mcfg->Length >= sizeof(ACPI_TABLE_HEADER) + 8 + sizeof(ACPI_MCFG_ENTRY)) {
        const ACPI_MCFG_ENTRY *Entry = (const ACPI_MCFG_ENTRY *)((const uint8_t *)Mcfg + sizeof(ACPI_TABLE_HEADER) + 8);
        if (Entry->Segment == 0 && Entry->BaseAddress) {
            PcipEcamBase = Entry->BaseAddress;
            PcipEcamStartBus = Entry->StartBus;
            PcipEcamEndBus = Entry->EndBus;
        }
    }
    PcipScanBus(0, 0);
    if (PcipEcamBase)
        DbgPrint("PCI: %u functions on %u bus(es), configuration through ECAM at 0x%llx\n", PcipDeviceCount,
                 PcipBusCount, (unsigned long long)PcipEcamBase);
    else
        DbgPrint("PCI: %u functions on %u bus(es), configuration through ports 0xCF8/0xCFC\n", PcipDeviceCount,
                 PcipBusCount);
    return STATUS_SUCCESS;
}

IO_DECLARE_BUILTIN_DRIVER(PciBuiltinDriver, "\\Driver\\Pci", PciDriverEntry, IO_ORDER_BUS);
