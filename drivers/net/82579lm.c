/*
 * Nexora OS - drivers/net/82579lm.c
 *
 * Identity fallback for Intel 82579LM (PCI 8086:1502). The E1000-family driver now has an
 * experimental, exact-revision/subsystem-gated bring-up path. Keep unmatched variants identity-only
 * and do not overwrite a started device's status.
 */
#include "ex.h"
#include "io.h"
#include "pci.h"

#define I82579LM_VENDOR_ID 0x8086
#define I82579LM_DEVICE_ID 0x1502
#define I82579LM_EXPECTED_SUBVENDOR 0x1028
#define I82579LM_EXPECTED_SUBDEVICE 0x052C
#define I82579LM_EXPECTED_REVISION 0x04
#define I82579LM_SUBSYSTEM_ID_OFFSET 0x2C

static STATUS I82579LMDriverEntry(DRIVER_OBJECT *Driver)
{
    uint32_t Identified = 0;

    for (PCI_DEVICE *Device = PciGetDeviceList(); Device; Device = Device->Next) {
        if (Device->VendorId != I82579LM_VENDOR_ID || Device->DeviceId != I82579LM_DEVICE_ID)
            continue;

        Identified++;
        if (Device->DriverState == PciDriverStarted)
            continue;
        PciSetDriverStatus(Device, PciDriverIdentityOnly, "Intel 82579LM (experimental)");
        if ((Device->HeaderType & 0x7F) != 0) {
            DbgPrintEx(DPFLTR_WARNING_LEVEL,
                       "82579LM: %02x:%02x.%u 8086:1502 has unsupported PCI header type %02x; not initialized\n",
                       Device->Bus, Device->Device, Device->Function, Device->HeaderType & 0x7F);
            continue;
        }

        uint32_t Subsystem = PciReadConfig32(Device, I82579LM_SUBSYSTEM_ID_OFFSET);
        uint16_t Subvendor = (uint16_t)Subsystem;
        uint16_t Subdevice = (uint16_t)(Subsystem >> 16);
        if (Subvendor == I82579LM_EXPECTED_SUBVENDOR && Subdevice == I82579LM_EXPECTED_SUBDEVICE &&
            Device->Revision == I82579LM_EXPECTED_REVISION) {
            DbgPrintEx(DPFLTR_WARNING_LEVEL,
                       "82579LM: experimental path did not start %02x:%02x.%u 8086:1502 subsystem "
                       "1028:052c rev 04; device remains unavailable\n",
                       Device->Bus, Device->Device, Device->Function);
        } else {
            DbgPrintEx(DPFLTR_WARNING_LEVEL,
                       "82579LM: identified %02x:%02x.%u 8086:1502 subsystem %04x:%04x rev %02x; "
                       "unsupported variant, not initialized\n",
                       Device->Bus, Device->Device, Device->Function, Subvendor, Subdevice, Device->Revision);
        }
    }

    return Identified ? STATUS_SUCCESS : STATUS_NO_SUCH_DEVICE;
}

IO_DECLARE_BUILTIN_DRIVER(I82579LMBuiltinDriver, "\\Driver\\I82579LM", I82579LMDriverEntry, IO_ORDER_DEVICE);
