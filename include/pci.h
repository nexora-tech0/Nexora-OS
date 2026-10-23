/*
 * Nexora OS - include/pci.h
 * PCI bus driver interface (drivers/pci): the enumerated device list, configuration space access
 * and MSI setup. Interrupt routing for legacy INTx needs the ACPI _PRT methods (AML), which
 * Nexora OS does not interpret, so drivers use MSI and fall back to polling without it.
 */
#ifndef PCI_H
#define PCI_H

#include "hzbase.h"

HZ_EXTERN_C_BEGIN

typedef struct _PCI_DEVICE {
    struct _PCI_DEVICE *Next;
    uint8_t Bus, Device, Function;
    uint8_t HeaderType;
    uint16_t VendorId, DeviceId;
    uint8_t Class, Subclass, ProgIf, Revision;
    uint8_t InterruptLine, InterruptPin;
    uint8_t MsiCapability;   /* config offset, 0 if none */
    uint8_t MsixCapability;
    uint64_t Bar[6];         /* base address (I/O port or physical memory), 0 if unused */
    uint64_t BarSize[6];
    bool BarIsIo[6];
} PCI_DEVICE;

/* PCI command register bits */
#define PCI_COMMAND_IO 0x0001
#define PCI_COMMAND_MEMORY 0x0002
#define PCI_COMMAND_BUS_MASTER 0x0004
#define PCI_COMMAND_INTX_DISABLE 0x0400

uint32_t PciReadConfig32(const PCI_DEVICE *Device, uint32_t Offset);
uint16_t PciReadConfig16(const PCI_DEVICE *Device, uint32_t Offset);
uint8_t PciReadConfig8(const PCI_DEVICE *Device, uint32_t Offset);
void PciWriteConfig32(const PCI_DEVICE *Device, uint32_t Offset, uint32_t Value);
void PciWriteConfig16(const PCI_DEVICE *Device, uint32_t Offset, uint16_t Value);

/* Enumeration result (valid after the PCI driver started, at IO_ORDER_BUS). */
PCI_DEVICE *PciGetDeviceList(void);
/* Next device after Previous (NULL = from the start) with this class/subclass/prog-if
   (0xFF = any prog-if). */
PCI_DEVICE *PciFindDevice(uint8_t Class, uint8_t Subclass, uint8_t ProgIf, PCI_DEVICE *Previous);
/* Sets bits in the command register (memory/I-O decoding, bus mastering). */
void PciEnableDevice(PCI_DEVICE *Device, uint16_t CommandBits);
/* Programs single-message MSI to Vector on the boot processor and masks INTx. */
STATUS PciEnableMsi(PCI_DEVICE *Device, uint8_t Vector);

HZ_EXTERN_C_END

#endif /* PCI_H */
