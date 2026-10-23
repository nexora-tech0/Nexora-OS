/*
 * Nexora OS - include/acpi.h
 * ACPI table layouts (from the ACPI specification) and the facts the HAL extracts from them.
 */
#ifndef ACPI_H
#define ACPI_H

#include "hzbase.h"

HZ_EXTERN_C_BEGIN

typedef struct HZ_PACKED _ACPI_RSDP {
    char Signature[8]; /* "RSD PTR " */
    uint8_t Checksum;  /* first 20 bytes */
    char OemId[6];
    uint8_t Revision;  /* 0 = ACPI 1.0 (RSDT only), 2+ = XSDT */
    uint32_t RsdtAddress;
    uint32_t Length;
    uint64_t XsdtAddress;
    uint8_t ExtendedChecksum;
    uint8_t Reserved[3];
} ACPI_RSDP;

typedef struct HZ_PACKED _ACPI_TABLE_HEADER {
    char Signature[4];
    uint32_t Length;
    uint8_t Revision;
    uint8_t Checksum;
    char OemId[6];
    char OemTableId[8];
    uint32_t OemRevision;
    uint32_t CreatorId;
    uint32_t CreatorRevision;
} ACPI_TABLE_HEADER;

typedef struct HZ_PACKED _ACPI_GAS { /* Generic Address Structure */
    uint8_t AddressSpace;  /* 0 = memory, 1 = I/O port */
    uint8_t BitWidth;
    uint8_t BitOffset;
    uint8_t AccessSize;
    uint64_t Address;
} ACPI_GAS;

#define ACPI_GAS_MEMORY 0
#define ACPI_GAS_IO 1

typedef struct HZ_PACKED _ACPI_MADT {
    ACPI_TABLE_HEADER Header;
    uint32_t LocalApicAddress;
    uint32_t Flags; /* bit 0: PC-AT compatible dual 8259 present */
    /* variable-length entries follow */
} ACPI_MADT;

#define MADT_TYPE_LOCAL_APIC 0
#define MADT_TYPE_IO_APIC 1
#define MADT_TYPE_INTERRUPT_OVERRIDE 2
#define MADT_TYPE_LOCAL_APIC_NMI 4
#define MADT_TYPE_LOCAL_APIC_OVERRIDE 5
#define MADT_TYPE_LOCAL_X2APIC 9
#define MADT_TYPE_LOCAL_X2APIC_NMI 0x0A

typedef struct HZ_PACKED _ACPI_FADT {
    ACPI_TABLE_HEADER Header;
    uint32_t FirmwareCtrl;
    uint32_t Dsdt;
    uint8_t Reserved0;
    uint8_t PreferredPmProfile;
    uint16_t SciInterrupt;
    uint32_t SmiCommandPort;
    uint8_t AcpiEnable;
    uint8_t AcpiDisable;
    uint8_t S4BiosReq;
    uint8_t PStateControl;
    uint32_t Pm1aEventBlock;
    uint32_t Pm1bEventBlock;
    uint32_t Pm1aControlBlock;
    uint32_t Pm1bControlBlock;
    uint32_t Pm2ControlBlock;
    uint32_t PmTimerBlock;
    uint32_t Gpe0Block;
    uint32_t Gpe1Block;
    uint8_t Pm1EventLength;
    uint8_t Pm1ControlLength;
    uint8_t Pm2ControlLength;
    uint8_t PmTimerLength;
    uint8_t Gpe0Length;
    uint8_t Gpe1Length;
    uint8_t Gpe1Base;
    uint8_t CStateControl;
    uint16_t WorstC2Latency;
    uint16_t WorstC3Latency;
    uint16_t FlushSize;
    uint16_t FlushStride;
    uint8_t DutyOffset;
    uint8_t DutyWidth;
    uint8_t DayAlarm;
    uint8_t MonthAlarm;
    uint8_t Century;
    uint16_t IapcBootArch; /* ACPI 2.0+ */
    uint8_t Reserved1;
    uint32_t Flags;
    ACPI_GAS ResetRegister;
    uint8_t ResetValue;
    uint16_t ArmBootArch;
    uint8_t MinorVersion;
    uint64_t XFirmwareCtrl;
    uint64_t XDsdt;
    ACPI_GAS XPm1aEventBlock;
    ACPI_GAS XPm1bEventBlock;
    ACPI_GAS XPm1aControlBlock;
    ACPI_GAS XPm1bControlBlock;
    ACPI_GAS XPm2ControlBlock;
    ACPI_GAS XPmTimerBlock;
    ACPI_GAS XGpe0Block;
    ACPI_GAS XGpe1Block;
} ACPI_FADT;

#define FADT_FLAG_TMR_VAL_EXT (1u << 8)   /* PM timer is 32 bits wide */
#define FADT_FLAG_RESET_REG_SUP (1u << 10)
#define FADT_BOOT_ARCH_8042 (1u << 1)

typedef struct HZ_PACKED _ACPI_HPET {
    ACPI_TABLE_HEADER Header;
    uint32_t EventTimerBlockId;
    ACPI_GAS BaseAddress;
    uint8_t HpetNumber;
    uint16_t MinimumTick;
    uint8_t PageProtection;
} ACPI_HPET;

/* ---- Facts extracted by hal/acpi.c --------------------------------------------------------- */

#define HAL_MAX_IOAPICS 8
#define HAL_MAX_OVERRIDES 16
#define HAL_MAX_CPUS 64

typedef struct _HAL_IOAPIC_INFO {
    uint8_t Id;
    uint32_t PhysicalAddress;
    uint32_t GsiBase;
} HAL_IOAPIC_INFO;

typedef struct _HAL_ISA_OVERRIDE {
    uint8_t IsaIrq;
    uint32_t Gsi;
    uint16_t Flags; /* MPS INTI flags: bits 0-1 polarity, bits 2-3 trigger */
} HAL_ISA_OVERRIDE;

typedef struct _HAL_LAPIC_NMI {
    uint32_t ProcessorUid; /* 0xFF / 0xFFFFFFFF = all processors */
    uint8_t Lint;
    uint16_t Flags;
} HAL_LAPIC_NMI;

typedef struct _HAL_ACPI_INFO {
    uint8_t RsdpRevision;
    char OemId[7];
    bool UsingXsdt;
    uint32_t TableCount;

    /* MADT */
    uint64_t LocalApicAddress;
    bool Has8259;
    uint32_t CpuCount;          /* enabled processors */
    uint32_t CpuApicIds[HAL_MAX_CPUS];
    uint32_t IoApicCount;
    HAL_IOAPIC_INFO IoApics[HAL_MAX_IOAPICS];
    uint32_t OverrideCount;
    HAL_ISA_OVERRIDE Overrides[HAL_MAX_OVERRIDES];
    uint32_t LapicNmiCount;
    HAL_LAPIC_NMI LapicNmis[4];

    /* FADT */
    bool HasFadt;
    uint16_t PmTimerPort;       /* 0 if none */
    bool PmTimer32Bit;
    uint8_t CenturyRegister;
    bool Has8042;
    bool HasResetRegister;
    ACPI_GAS ResetRegister;
    uint8_t ResetValue;
    uint16_t SciInterrupt;
    uint16_t Pm1aControlPort;   /* 0 if none */
    uint16_t Pm1bControlPort;
    uint16_t SmiCommandPort;
    uint8_t AcpiEnableValue;
    uint64_t DsdtAddress;       /* physical */
    /* \_S5 sleep types from the DSDT (power off) */
    bool HasS5;
    uint8_t S5SleepTypeA;
    uint8_t S5SleepTypeB;

    /* HPET */
    uint64_t HpetAddress;       /* 0 if none */
} HAL_ACPI_INFO;

extern HAL_ACPI_INFO HalAcpi;

HZ_EXTERN_C_END

#endif /* ACPI_H */
