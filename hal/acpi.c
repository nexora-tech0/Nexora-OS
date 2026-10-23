/*
 * Nexora OS - hal/acpi.c
 * ACPI static table discovery: RSDP -> XSDT (or RSDT) -> MADT, FADT, HPET. Every table is
 * checksum-verified; tables with bad checksums are logged and ignored. With Limine base
 * revision 6 the RSDP pointer is an HHDM virtual address and all RSDT/XSDT-referenced tables
 * are inside HHDM-mapped memory, so table physical addresses are reached through the HHDM.
 * AML (DSDT/SSDT) interpretation is out of scope; S5 power-off (M5) uses a minimal \_S5 scan.
 */
#include "acpi.h"
#include "bootinfo.h"
#include "halp.h"
#include "rtl.h"

_Static_assert(offsetof(ACPI_FADT, PmTimerBlock) == 76, "FADT layout");
_Static_assert(offsetof(ACPI_FADT, Century) == 108, "FADT layout");
_Static_assert(offsetof(ACPI_FADT, Flags) == 112, "FADT layout");
_Static_assert(offsetof(ACPI_FADT, ResetRegister) == 116, "FADT layout");
_Static_assert(offsetof(ACPI_FADT, XDsdt) == 140, "FADT layout");
_Static_assert(offsetof(ACPI_FADT, XPmTimerBlock) == 208, "FADT layout");
_Static_assert(sizeof(ACPI_FADT) == 244, "FADT layout");

#define HAL_MAX_ACPI_TABLES 64

HAL_ACPI_INFO HalAcpi;
static const ACPI_TABLE_HEADER *HalpTables[HAL_MAX_ACPI_TABLES];

#define READ_UNALIGNED(Type, Pointer)                                                          \
    ({                                                                                         \
        Type Value_;                                                                           \
        memcpy(&Value_, (Pointer), sizeof(Type));                                              \
        Value_;                                                                                \
    })

static const void *HalpPhysicalToVirtual(uint64_t Physical)
{
    return (const void *)(Physical + KeLoaderBlock.HhdmOffset);
}

static bool HalpChecksumOk(const void *Data, uint32_t Length)
{
    const uint8_t *p = (const uint8_t *)Data;
    uint8_t Sum = 0;
    for (uint32_t i = 0; i < Length; i++)
        Sum = (uint8_t)(Sum + p[i]);
    return Sum == 0;
}

static void HalpAddTable(uint64_t Physical)
{
    if (Physical == 0 || HalAcpi.TableCount >= HAL_MAX_ACPI_TABLES)
        return;
    const ACPI_TABLE_HEADER *T = (const ACPI_TABLE_HEADER *)HalpPhysicalToVirtual(Physical);
    if (T->Length < sizeof(ACPI_TABLE_HEADER) || T->Length > MiB(1) || !HalpChecksumOk(T, T->Length)) {
        DbgPrintEx(DPFLTR_WARNING_LEVEL, "ACPI: ignoring table %.4s at 0x%llx (bad length or checksum)\n",
                   T->Signature, (unsigned long long)Physical);
        return;
    }
    HalpTables[HalAcpi.TableCount++] = T;
}

const void *HalAcpiFindTable(const char *Signature, uint32_t Index)
{
    for (uint32_t i = 0; i < HalAcpi.TableCount; i++) {
        if (memcmp(HalpTables[i]->Signature, Signature, 4) == 0 && Index-- == 0)
            return HalpTables[i];
    }
    return NULL;
}

static void HalpParseMadt(const ACPI_MADT *Madt)
{
    HalAcpi.LocalApicAddress = Madt->LocalApicAddress;
    HalAcpi.Has8259 = (Madt->Flags & 1) != 0;

    const uint8_t *p = (const uint8_t *)(Madt + 1);
    const uint8_t *End = (const uint8_t *)Madt + Madt->Header.Length;
    while (p + 2 <= End && p[1] >= 2 && p + p[1] <= End) {
        uint8_t Type = p[0];
        switch (Type) {
        case MADT_TYPE_LOCAL_APIC: {
            uint32_t Flags = READ_UNALIGNED(uint32_t, p + 4);
            if ((Flags & 1) && HalAcpi.CpuCount < HAL_MAX_CPUS)
                HalAcpi.CpuApicIds[HalAcpi.CpuCount++] = p[3];
            break;
        }
        case MADT_TYPE_LOCAL_X2APIC: {
            uint32_t Flags = READ_UNALIGNED(uint32_t, p + 8);
            if ((Flags & 1) && HalAcpi.CpuCount < HAL_MAX_CPUS)
                HalAcpi.CpuApicIds[HalAcpi.CpuCount++] = READ_UNALIGNED(uint32_t, p + 4);
            break;
        }
        case MADT_TYPE_IO_APIC:
            if (HalAcpi.IoApicCount < HAL_MAX_IOAPICS) {
                HAL_IOAPIC_INFO *Io = &HalAcpi.IoApics[HalAcpi.IoApicCount++];
                Io->Id = p[2];
                Io->PhysicalAddress = READ_UNALIGNED(uint32_t, p + 4);
                Io->GsiBase = READ_UNALIGNED(uint32_t, p + 8);
            }
            break;
        case MADT_TYPE_INTERRUPT_OVERRIDE:
            if (p[2] == 0 && HalAcpi.OverrideCount < HAL_MAX_OVERRIDES) { /* bus 0 = ISA */
                HAL_ISA_OVERRIDE *O = &HalAcpi.Overrides[HalAcpi.OverrideCount++];
                O->IsaIrq = p[3];
                O->Gsi = READ_UNALIGNED(uint32_t, p + 4);
                O->Flags = READ_UNALIGNED(uint16_t, p + 8);
            }
            break;
        case MADT_TYPE_LOCAL_APIC_NMI:
            if (HalAcpi.LapicNmiCount < HZ_ARRAYSIZE(HalAcpi.LapicNmis)) {
                HAL_LAPIC_NMI *N = &HalAcpi.LapicNmis[HalAcpi.LapicNmiCount++];
                N->ProcessorUid = p[2] == 0xFF ? 0xFFFFFFFFu : p[2];
                N->Flags = READ_UNALIGNED(uint16_t, p + 3);
                N->Lint = p[5];
            }
            break;
        case MADT_TYPE_LOCAL_X2APIC_NMI:
            if (HalAcpi.LapicNmiCount < HZ_ARRAYSIZE(HalAcpi.LapicNmis)) {
                HAL_LAPIC_NMI *N = &HalAcpi.LapicNmis[HalAcpi.LapicNmiCount++];
                N->Flags = READ_UNALIGNED(uint16_t, p + 2);
                N->ProcessorUid = READ_UNALIGNED(uint32_t, p + 4);
                N->Lint = p[8];
            }
            break;
        case MADT_TYPE_LOCAL_APIC_OVERRIDE:
            HalAcpi.LocalApicAddress = READ_UNALIGNED(uint64_t, p + 4);
            break;
        default:
            break;
        }
        p += p[1];
    }
}

static void HalpParseFadt(const ACPI_FADT *Fadt)
{
    uint32_t Length = Fadt->Header.Length;
    HalAcpi.HasFadt = true;
    HalAcpi.SciInterrupt = Fadt->SciInterrupt;
    HalAcpi.CenturyRegister = Fadt->Century;
    /* IAPC_BOOT_ARCH only exists from FADT revision 3 (ACPI 2.0); assume an 8042 before that. */
    HalAcpi.Has8042 = Fadt->Header.Revision < 3 || (Fadt->IapcBootArch & FADT_BOOT_ARCH_8042) != 0;

    if (Fadt->PmTimerLength == 4) {
        uint64_t Port = Fadt->PmTimerBlock;
        if (Length >= offsetof(ACPI_FADT, XPmTimerBlock) + sizeof(ACPI_GAS) &&
            Fadt->XPmTimerBlock.Address != 0 && Fadt->XPmTimerBlock.AddressSpace == ACPI_GAS_IO)
            Port = Fadt->XPmTimerBlock.Address;
        if (Port != 0 && Port <= 0xFFFF) {
            HalAcpi.PmTimerPort = (uint16_t)Port;
            HalAcpi.PmTimer32Bit = (Fadt->Flags & FADT_FLAG_TMR_VAL_EXT) != 0;
        }
    }

    if (Length >= offsetof(ACPI_FADT, ResetValue) + 1 && (Fadt->Flags & FADT_FLAG_RESET_REG_SUP)) {
        HalAcpi.HasResetRegister = true;
        HalAcpi.ResetRegister = Fadt->ResetRegister;
        HalAcpi.ResetValue = Fadt->ResetValue;
    }

    /* PM1 control blocks (power off), the SMI command port (switching to ACPI mode), DSDT. */
    HalAcpi.Pm1aControlPort = (uint16_t)Fadt->Pm1aControlBlock;
    HalAcpi.Pm1bControlPort = (uint16_t)Fadt->Pm1bControlBlock;
    if (Length >= offsetof(ACPI_FADT, XPm1bControlBlock) + sizeof(ACPI_GAS)) {
        if (Fadt->XPm1aControlBlock.AddressSpace == ACPI_GAS_IO && Fadt->XPm1aControlBlock.Address &&
            Fadt->XPm1aControlBlock.Address <= 0xFFFF)
            HalAcpi.Pm1aControlPort = (uint16_t)Fadt->XPm1aControlBlock.Address;
        if (Fadt->XPm1bControlBlock.AddressSpace == ACPI_GAS_IO && Fadt->XPm1bControlBlock.Address &&
            Fadt->XPm1bControlBlock.Address <= 0xFFFF)
            HalAcpi.Pm1bControlPort = (uint16_t)Fadt->XPm1bControlBlock.Address;
    }
    HalAcpi.SmiCommandPort = (uint16_t)Fadt->SmiCommandPort;
    HalAcpi.AcpiEnableValue = Fadt->AcpiEnable;
    HalAcpi.DsdtAddress = Fadt->Dsdt;
    if (Length >= offsetof(ACPI_FADT, XDsdt) + 8 && Fadt->XDsdt)
        HalAcpi.DsdtAddress = Fadt->XDsdt;
}

/* One AML integer constant (ZeroOp, OneOp, OnesOp, BytePrefix, WordPrefix); false if anything else. */
static bool HalpParseAmlInteger(const uint8_t **Cursor, const uint8_t *End, uint8_t *Value)
{
    const uint8_t *p = *Cursor;
    if (p >= End)
        return false;
    switch (*p) {
    case 0x00: *Value = 0; p += 1; break;          /* ZeroOp */
    case 0x01: *Value = 1; p += 1; break;          /* OneOp */
    case 0xFF: *Value = 0xFF; p += 1; break;       /* OnesOp */
    case 0x0A:                                     /* BytePrefix */
        if (p + 2 > End)
            return false;
        *Value = p[1];
        p += 2;
        break;
    case 0x0B:                                     /* WordPrefix (low byte is enough) */
        if (p + 3 > End)
            return false;
        *Value = p[1];
        p += 3;
        break;
    default:
        return false;
    }
    *Cursor = p;
    return true;
}

/*
 * Finds the \_S5 package in the DSDT without an AML interpreter: the pattern is
 * NameOp [RootChar] "_S5_" PackageOp PkgLength NumElements SLP_TYPa SLP_TYPb ... Every firmware
 * we target (OVMF, VirtualBox) declares it this way.
 */
static void HalpFindS5(void)
{
    if (!HalAcpi.DsdtAddress)
        return;
    const ACPI_TABLE_HEADER *Dsdt = HalpPhysicalToVirtual(HalAcpi.DsdtAddress);
    if (memcmp(Dsdt->Signature, "DSDT", 4) != 0 || !HalpChecksumOk(Dsdt, Dsdt->Length))
        return;
    const uint8_t *Start = (const uint8_t *)(Dsdt + 1);
    const uint8_t *End = (const uint8_t *)Dsdt + Dsdt->Length;
    for (const uint8_t *p = Start + 1; p + 8 < End; p++) {
        if (memcmp(p, "_S5_", 4) != 0)
            continue;
        if (p[-1] != 0x08 && !(p[-1] == '\\' && p - 2 >= Start && p[-2] == 0x08))
            continue; /* not a NameOp declaration */
        const uint8_t *q = p + 4;
        if (*q++ != 0x12) /* PackageOp */
            continue;
        q += 1 + (*q >> 6); /* PkgLength: lead byte + (bits 7:6) following bytes */
        q += 1;             /* NumElements */
        uint8_t A, B;
        if (HalpParseAmlInteger(&q, End, &A) && HalpParseAmlInteger(&q, End, &B)) {
            HalAcpi.HasS5 = true;
            HalAcpi.S5SleepTypeA = A & 7;
            HalAcpi.S5SleepTypeB = B & 7;
        }
        return;
    }
}

void HalInitializeAcpi(void)
{
    const ACPI_RSDP *Rsdp = (const ACPI_RSDP *)KeLoaderBlock.Rsdp;
    if (!Rsdp || memcmp(Rsdp->Signature, "RSD PTR ", 8) != 0 || !HalpChecksumOk(Rsdp, 20))
        KeBugCheckEx(HAL_INITIALIZATION_FAILED, HAL_FAILURE_NO_RSDP, (uint64_t)Rsdp, 0, 0);

    HalAcpi.RsdpRevision = Rsdp->Revision;
    memcpy(HalAcpi.OemId, Rsdp->OemId, 6);
    HalAcpi.OemId[6] = '\0';

    if (Rsdp->Revision >= 2 && Rsdp->XsdtAddress != 0 && HalpChecksumOk(Rsdp, Rsdp->Length)) {
        const ACPI_TABLE_HEADER *Xsdt = HalpPhysicalToVirtual(Rsdp->XsdtAddress);
        if (memcmp(Xsdt->Signature, "XSDT", 4) != 0 || !HalpChecksumOk(Xsdt, Xsdt->Length))
            KeBugCheckEx(HAL_INITIALIZATION_FAILED, HAL_FAILURE_BAD_ACPI_TABLES, Rsdp->XsdtAddress, 0, 0);
        HalAcpi.UsingXsdt = true;
        uint32_t Count = (Xsdt->Length - sizeof(ACPI_TABLE_HEADER)) / 8;
        const uint8_t *Entries = (const uint8_t *)(Xsdt + 1);
        for (uint32_t i = 0; i < Count; i++)
            HalpAddTable(READ_UNALIGNED(uint64_t, Entries + i * 8));
    } else {
        const ACPI_TABLE_HEADER *Rsdt = HalpPhysicalToVirtual(Rsdp->RsdtAddress);
        if (memcmp(Rsdt->Signature, "RSDT", 4) != 0 || !HalpChecksumOk(Rsdt, Rsdt->Length))
            KeBugCheckEx(HAL_INITIALIZATION_FAILED, HAL_FAILURE_BAD_ACPI_TABLES, Rsdp->RsdtAddress, 0, 0);
        uint32_t Count = (Rsdt->Length - sizeof(ACPI_TABLE_HEADER)) / 4;
        const uint8_t *Entries = (const uint8_t *)(Rsdt + 1);
        for (uint32_t i = 0; i < Count; i++)
            HalpAddTable(READ_UNALIGNED(uint32_t, Entries + i * 4));
    }

    char Names[HAL_MAX_ACPI_TABLES * 5 + 1];
    size_t n = 0;
    for (uint32_t i = 0; i < HalAcpi.TableCount; i++) {
        memcpy(Names + n, HalpTables[i]->Signature, 4);
        Names[n + 4] = ' ';
        n += 5;
    }
    Names[n ? n - 1 : 0] = '\0';
    DbgPrint("ACPI: revision %u, OEM \"%s\", %s with %u tables: %s\n", HalAcpi.RsdpRevision, HalAcpi.OemId,
             HalAcpi.UsingXsdt ? "XSDT" : "RSDT", HalAcpi.TableCount, Names);

    const ACPI_MADT *Madt = HalAcpiFindTable("APIC", 0);
    if (!Madt)
        KeBugCheckEx(HAL_INITIALIZATION_FAILED, HAL_FAILURE_NO_MADT, 0, 0, 0);
    HalpParseMadt(Madt);
    if (HalAcpi.IoApicCount == 0)
        KeBugCheckEx(HAL_INITIALIZATION_FAILED, HAL_FAILURE_NO_IOAPIC, 0, 0, 0);

    const ACPI_FADT *Fadt = HalAcpiFindTable("FACP", 0);
    if (Fadt) {
        HalpParseFadt(Fadt);
        HalpFindS5();
    }

    const ACPI_HPET *Hpet = HalAcpiFindTable("HPET", 0);
    if (Hpet && Hpet->BaseAddress.AddressSpace == ACPI_GAS_MEMORY && Hpet->BaseAddress.Address != 0)
        HalAcpi.HpetAddress = Hpet->BaseAddress.Address;

    DbgPrint("ACPI: %u CPU(s), Local APIC 0x%llx, %s8259 PICs\n", HalAcpi.CpuCount,
             (unsigned long long)HalAcpi.LocalApicAddress, HalAcpi.Has8259 ? "" : "no ");
    for (uint32_t i = 0; i < HalAcpi.IoApicCount; i++)
        DbgPrint("ACPI: I/O APIC %u at 0x%x, GSI base %u\n", HalAcpi.IoApics[i].Id,
                 HalAcpi.IoApics[i].PhysicalAddress, HalAcpi.IoApics[i].GsiBase);
    for (uint32_t i = 0; i < HalAcpi.OverrideCount; i++) {
        const HAL_ISA_OVERRIDE *O = &HalAcpi.Overrides[i];
        DbgPrintEx(DPFLTR_TRACE_LEVEL, "ACPI: override IRQ %u -> GSI %u (flags 0x%x)\n", O->IsaIrq, O->Gsi,
                   O->Flags);
    }
    char PmTimer[32] = "none";
    if (HalAcpi.PmTimerPort)
        RtlFormatString(PmTimer, sizeof(PmTimer), "port 0x%x (%s)", HalAcpi.PmTimerPort,
                        HalAcpi.PmTimer32Bit ? "32-bit" : "24-bit");
    DbgPrint("ACPI: PM timer %s, HPET %s, century register %s, 8042 %s\n", PmTimer,
             HalAcpi.HpetAddress ? "present" : "absent", HalAcpi.CenturyRegister ? "present" : "absent",
             HalAcpi.Has8042 ? "present" : "not reported");
    DbgPrintEx(DPFLTR_TRACE_LEVEL, "ACPI: PM1a control 0x%x, PM1b control 0x%x, reset register %s, \\_S5 %s (%u/%u)\n",
               HalAcpi.Pm1aControlPort, HalAcpi.Pm1bControlPort, HalAcpi.HasResetRegister ? "present" : "absent",
               HalAcpi.HasS5 ? "found" : "not found", HalAcpi.S5SleepTypeA, HalAcpi.S5SleepTypeB);
}
