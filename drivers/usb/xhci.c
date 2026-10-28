/*
 * Nexora OS - drivers/usb/xhci.c
 * xHCI bring-up with polled command/event rings, root-port enumeration, boot HID keyboard/mouse,
 * and USB mass-storage Bulk-Only Transport. Hub support, hotplug, SuperSpeed tuning, generic HID
 * report descriptors, and UASP remain outside this implementation.
 */
#include "ex.h"
#include "io.h"
#include "ke.h"
#include "mm.h"
#include "pci.h"
#include "ps.h"
#include "rtl.h"
#include "input.h"
#include "disk.h"

#define XHCI_CAPLENGTH 0x00u
#define XHCI_HCIVERSION 0x02u
#define XHCI_HCSPARAMS1 0x04u
#define XHCI_HCCPARAMS1 0x10u
#define XHCI_DBOFF 0x14u
#define XHCI_RTSOFF 0x18u
#define XHCI_USBCMD 0x00u
#define XHCI_USBSTS 0x04u
#define XHCI_CMD_RUN (1u << 0)
#define XHCI_CMD_RESET (1u << 1)
#define XHCI_STS_HALTED (1u << 0)
#define XHCI_STS_NOT_READY (1u << 11)
#define XHCI_STS_HALTED_BIT 0u
#define XHCI_CAP_MAX_SLOTS 0x04u
#define XHCI_CAP_MAX_PORTS 0x04u
#define XHCI_OP_CONFIG 0x38u
#define XHCI_OP_PORTS 0x400u
#define XHCI_RT_INTERRUPTER0 0x20u
#define XHCI_RT_ERSTSZ 0x08u
#define XHCI_RT_ERSTBA 0x10u
#define XHCI_RT_ERDP 0x18u
#define XHCI_PORT_CCS (1u << 0)
#define XHCI_PORT_PED (1u << 1)
#define XHCI_PORT_PR (1u << 4)
#define XHCI_PORT_PP (1u << 9)
#define XHCI_PORT_SPEED_MASK (0xFu << 10)
#define XHCI_PORT_PRC (1u << 21)
#define XHCI_PORT_WRC (1u << 19)
#define XHCI_PORT_WPR (1u << 31)
#define XHCI_TRB_LINK 6u
#define XHCI_TRB_ENABLE_SLOT 9u
#define XHCI_TRB_COMMAND_COMPLETION 33u
#define XHCI_TRB_PORT_STATUS_CHANGE 34u
#define XHCI_TRB_TRANSFER_EVENT 32u
#define XHCI_TRB_NORMAL 1u
#define XHCI_TRB_SETUP_STAGE 2u
#define XHCI_TRB_DATA_STAGE 3u
#define XHCI_TRB_STATUS_STAGE 4u
#define XHCI_TRB_ADDRESS_DEVICE 11u
#define XHCI_TRB_EVALUATE_CONTEXT 13u
#define XHCI_TRB_DISABLE_SLOT 10u
#define XHCI_TRB_CONFIGURE_ENDPOINT 12u
#define XHCI_TRB_CYCLE (1u << 0)
#define XHCI_TRB_TOGGLE_CYCLE (1u << 1)
#define XHCI_TRB_CHAIN (1u << 4)
#define XHCI_TRB_IOC (1u << 5)
#define XHCI_TRB_IDT (1u << 6)
#define XHCI_TRB_DIR_IN (1u << 16)
#define XHCI_TRB_TRT_IN_DATA (3u << 16)
#define XHCI_TRB_TYPE_SHIFT 10u
#define XHCI_TRB_TYPE_MASK (0x3Fu << XHCI_TRB_TYPE_SHIFT)
#define XHCI_TRB_COMPLETION_SHIFT 24u
#define XHCI_TRB_SLOT_SHIFT 24u
#define XHCI_TRB_SUCCESS 1u
#define XHCI_TRB_SHORT_PACKET 13u
#define XHCI_DMA_PAGES 4u
#define XHCI_DMA_DCBAA_PAGE 0u
#define XHCI_DMA_COMMAND_PAGE 1u
#define XHCI_DMA_EVENT_PAGE 2u
#define XHCI_DMA_ERST_PAGE 3u
#define XHCI_DEVICE_DMA_PAGES 10u
#define XHCI_DEVICE_OUTPUT_CONTEXT_PAGE 0u
#define XHCI_DEVICE_INPUT_CONTEXT_PAGE 1u
#define XHCI_DEVICE_EP0_RING_PAGE 2u
#define XHCI_DEVICE_DESCRIPTOR_PAGE 3u
#define XHCI_DEVICE_CONFIG_PAGE 4u
#define XHCI_DEVICE_INTERRUPT_RING_PAGE 5u
#define XHCI_DEVICE_REPORT_PAGE 6u
#define XHCI_DEVICE_BULK_IN_RING_PAGE 7u
#define XHCI_DEVICE_BULK_OUT_RING_PAGE 8u
#define XHCI_DEVICE_BULK_BUFFER_PAGE 9u
#define XHCI_RING_TRBS (PAGE_SIZE / 16u)
#define XHCI_MAX_CONTROLLERS 16u
#define XHCI_MAX_PORTS 255u
#define XHCI_MAX_ACTIVE_DEVICES 8u
#define XHCI_TIMEOUT_US 1000000u
#define XHCI_EXT_USB_LEGACY 1u
#define XHCI_EXT_BIOS_OWNED (1u << 16)
#define XHCI_EXT_OS_OWNED (1u << 24)

typedef struct _XHCI_DEVICE {
    bool Active;
    bool IsKeyboard;
    bool IsMouse;
    bool IsMassStorage;
    uint8_t SlotId;
    uint8_t Port;
    uint8_t Speed;
    uint8_t InterfaceNumber;
    uint8_t EndpointAddress;
    uint8_t EndpointDci;
    uint8_t Configuration;
    uint8_t Interval;
    uint16_t MaxPacket;
    PFN_NUMBER DmaPfn;
    uint8_t *Dma;
    volatile uint32_t *Ep0Ring;
    uint32_t Ep0Index;
    uint8_t Ep0Cycle;
    uint8_t PreviousReport[8];
    uint8_t Modifiers;
    uint8_t LockModifiers;
    uint8_t ReportBytes;
    uint32_t InterruptIndex;
    uint8_t InterruptCycle;
    volatile uint32_t *InterruptRing;
    uint8_t BulkInDci, BulkOutDci;
    uint16_t BulkInPacket, BulkOutPacket;
    uint8_t BulkInCycle, BulkOutCycle;
    uint32_t BulkInIndex, BulkOutIndex;
    uint32_t MaxLun;
    uint64_t SectorCount;
    struct _XHCI_CONTROLLER *Controller;
} XHCI_DEVICE;

typedef struct _XHCI_CONTROLLER {
    PCI_DEVICE *Pci;
    volatile uint8_t *Registers;
    uint64_t RegisterBytes;
    uint8_t CapabilityLength;
    uint32_t DoorbellOffset;
    uint32_t RuntimeOffset;
    uint32_t PortCount;
    uint32_t MaxSlots;
    uint32_t ActiveSlots;
    PFN_NUMBER DmaPfn;
    uint8_t *Dma;
    volatile uint32_t *Doorbells;
    volatile uint32_t *Operational;
    volatile uint64_t *Dcbaa;
    volatile uint32_t *CommandRing;
    volatile uint32_t *EventRing;
    uint32_t CommandIndex;
    uint32_t EventIndex;
    uint8_t CommandCycle;
    uint8_t EventCycle;
    KMUTEX EventLock;
    XHCI_DEVICE Devices[XHCI_MAX_ACTIVE_DEVICES];
} XHCI_CONTROLLER;

typedef struct HZ_PACKED _XHCI_ERST_ENTRY {
    uint64_t RingBase;
    uint32_t RingSize;
    uint32_t Reserved;
} XHCI_ERST_ENTRY;

typedef struct HZ_PACKED _XHCI_TRB {
    uint32_t ParameterLow;
    uint32_t ParameterHigh;
    uint32_t Status;
    uint32_t Control;
} XHCI_TRB;

_Static_assert(sizeof(XHCI_ERST_ENTRY) == 16, "xHCI ERST entry size");
_Static_assert(sizeof(XHCI_TRB) == 16, "xHCI TRB size");

static XHCI_CONTROLLER XhciControllers[XHCI_MAX_CONTROLLERS];
static uint32_t XhciControllerCount;

static uint32_t XhciRead32(XHCI_CONTROLLER *Controller, uint32_t Offset)
{
    return *(volatile uint32_t *)(Controller->Registers + Offset);
}

static void XhciWrite32(XHCI_CONTROLLER *Controller, uint32_t Offset, uint32_t Value)
{
    *(volatile uint32_t *)(Controller->Registers + Offset) = Value;
}

static void XhciWrite64(XHCI_CONTROLLER *Controller, uint32_t Offset, uint64_t Value)
{
    *(volatile uint64_t *)(Controller->Registers + Offset) = Value;
}

static bool XhciWait32(XHCI_CONTROLLER *Controller, uint32_t Offset, uint32_t Mask, uint32_t Expected)
{
    for (uint32_t Elapsed = 0; Elapsed < XHCI_TIMEOUT_US; Elapsed += 10) {
        if ((XhciRead32(Controller, Offset) & Mask) == Expected)
            return true;
        KeStallExecutionProcessor(10);
    }
    return (XhciRead32(Controller, Offset) & Mask) == Expected;
}

static bool XhciHandoffFirmware(XHCI_CONTROLLER *Controller)
{
    uint32_t ExtendedOffset = ((XhciRead32(Controller, XHCI_HCCPARAMS1) >> 16) & 0xFFFFu) * 4u;
    uint32_t Visited[64];
    uint32_t VisitedCount = 0;

    while (ExtendedOffset) {
        if (ExtendedOffset > Controller->RegisterBytes - sizeof(uint32_t) ||
            (ExtendedOffset & 3u) || VisitedCount == sizeof(Visited) / sizeof(Visited[0]))
            return false;
        for (uint32_t i = 0; i < VisitedCount; i++) {
            if (Visited[i] == ExtendedOffset)
                return false;
        }
        Visited[VisitedCount++] = ExtendedOffset;

        uint32_t Header = XhciRead32(Controller, ExtendedOffset);
        uint8_t CapabilityId = (uint8_t)Header;
        uint8_t Next = (uint8_t)(Header >> 8);
        if (CapabilityId == XHCI_EXT_USB_LEGACY) {
            if (Header & XHCI_EXT_BIOS_OWNED) {
                XhciWrite32(Controller, ExtendedOffset, Header | XHCI_EXT_OS_OWNED);
                if (!XhciWait32(Controller, ExtendedOffset, XHCI_EXT_BIOS_OWNED, 0))
                    return false;
            }
            return true;
        }
        if (!Next)
            break;
        ExtendedOffset += (uint32_t)Next * 4u;
    }
    return true;
}

static uint64_t XhciDmaPhysical(XHCI_CONTROLLER *Controller, uint32_t Page)
{
    return (Controller->DmaPfn + Page) << PAGE_SHIFT;
}

static uint64_t XhciDeviceDmaPhysical(XHCI_DEVICE *Device, uint32_t Page)
{
    return (Device->DmaPfn + Page) << PAGE_SHIFT;
}

static void XhciUpdateEventDequeue(XHCI_CONTROLLER *Controller)
{
    uint64_t Address = XhciDmaPhysical(Controller, XHCI_DMA_EVENT_PAGE) +
                       Controller->EventIndex * sizeof(XHCI_TRB);
    uint32_t ErdpOffset = Controller->RuntimeOffset + XHCI_RT_INTERRUPTER0 + XHCI_RT_ERDP;
    XhciWrite64(Controller, ErdpOffset, Address | (1u << 3)); /* EHB */
}

static bool XhciPopEvent(XHCI_CONTROLLER *Controller, XHCI_TRB *Event)
{
    volatile XHCI_TRB *Ring = (volatile XHCI_TRB *)Controller->EventRing;
    uint32_t Control = Ring[Controller->EventIndex].Control;
    if ((Control & XHCI_TRB_CYCLE) != Controller->EventCycle)
        return false;

    Event->ParameterLow = Ring[Controller->EventIndex].ParameterLow;
    Event->ParameterHigh = Ring[Controller->EventIndex].ParameterHigh;
    Event->Status = Ring[Controller->EventIndex].Status;
    Event->Control = Control;
    Controller->EventIndex++;
    if (Controller->EventIndex == XHCI_RING_TRBS) {
        Controller->EventIndex = 0;
        Controller->EventCycle ^= 1;
    }
    XhciUpdateEventDequeue(Controller);
    return true;
}

static bool XhciSubmitCommand(XHCI_CONTROLLER *Controller, XHCI_TRB *Command, XHCI_TRB *Completion)
{
    volatile XHCI_TRB *Ring = (volatile XHCI_TRB *)Controller->CommandRing;
    uint32_t Index = Controller->CommandIndex;
    Command->Control = (Command->Control & ~XHCI_TRB_CYCLE) |
                       (Controller->CommandCycle ? XHCI_TRB_CYCLE : 0);
    Ring[Index] = *Command;
    __asm__ volatile("" : : : "memory");

    Controller->CommandIndex++;
    if (Controller->CommandIndex == XHCI_RING_TRBS - 1) {
        XHCI_TRB Link = {0};
        uint64_t Base = XhciDmaPhysical(Controller, XHCI_DMA_COMMAND_PAGE);
        Link.ParameterLow = (uint32_t)Base;
        Link.ParameterHigh = (uint32_t)(Base >> 32);
        Link.Control = (XHCI_TRB_LINK << XHCI_TRB_TYPE_SHIFT) | XHCI_TRB_TOGGLE_CYCLE |
                       (Controller->CommandCycle ? XHCI_TRB_CYCLE : 0);
        Ring[Controller->CommandIndex] = Link;
        Controller->CommandIndex = 0;
        Controller->CommandCycle ^= 1;
    }

    Controller->Doorbells[0] = 0;
    for (uint32_t Elapsed = 0; Elapsed < XHCI_TIMEOUT_US; Elapsed += 100) {
        XHCI_TRB Event;
        if (XhciPopEvent(Controller, &Event)) {
            uint32_t Type = (Event.Control & XHCI_TRB_TYPE_MASK) >> XHCI_TRB_TYPE_SHIFT;
            if (Type == XHCI_TRB_COMMAND_COMPLETION) {
                *Completion = Event;
                uint32_t Code = (Event.Status >> XHCI_TRB_COMPLETION_SHIFT) & 0xFF;
                if (Code != XHCI_TRB_SUCCESS)
                    DbgPrintEx(DPFLTR_WARNING_LEVEL, "xHCI: command type %u completion code %u, slot %u\n",
                               (Command->Control & XHCI_TRB_TYPE_MASK) >> XHCI_TRB_TYPE_SHIFT,
                               Code, (uint8_t)(Event.Control >> XHCI_TRB_SLOT_SHIFT));
                return Code == XHCI_TRB_SUCCESS;
            }
            if (Type != XHCI_TRB_PORT_STATUS_CHANGE)
                DbgPrintEx(DPFLTR_WARNING_LEVEL, "xHCI: ignored event TRB type %u while waiting for slot\n", Type);
        } else {
            KeStallExecutionProcessor(100);
        }
    }
    return false;
}

static bool XhciSubmitEnableSlot(XHCI_CONTROLLER *Controller, uint8_t *SlotId)
{
    XHCI_TRB Command = {0};
    XHCI_TRB Completion = {0};
    Command.Control = XHCI_TRB_ENABLE_SLOT << XHCI_TRB_TYPE_SHIFT;
    if (!XhciSubmitCommand(Controller, &Command, &Completion))
        return false;
    *SlotId = (uint8_t)(Completion.Control >> XHCI_TRB_SLOT_SHIFT);
    return *SlotId != 0;
}

static uint32_t *XhciInputContext(XHCI_DEVICE *Device)
{
    return (uint32_t *)(Device->Dma + XHCI_DEVICE_INPUT_CONTEXT_PAGE * PAGE_SIZE);
}

static uint32_t XhciContextBytes(XHCI_CONTROLLER *Controller)
{
    return (XhciRead32(Controller, XHCI_HCCPARAMS1) & (1u << 2)) ? 64u : 32u;
}

static void XhciWriteInputSlot(XHCI_CONTROLLER *Controller, XHCI_DEVICE *Device, uint32_t ContextBytes)
{
    uint32_t *Input = XhciInputContext(Device);
    uint32_t *Slot = (uint32_t *)((uint8_t *)Input + ContextBytes);
    uint32_t *Ep0 = (uint32_t *)((uint8_t *)Slot + ContextBytes);
    memset(Input, 0, PAGE_SIZE);
    Input[1] = 1u | (1u << 1); /* Add Slot Context and Endpoint 0 Context. */
    Slot[0] = ((uint32_t)Device->Speed << 20) | (1u << 27); /* Context Entries = 1. */
    Slot[1] = (uint32_t)Device->Port << 16;
    uint32_t InitialMaxPacket = Device->Speed == 3 ? 64u : 8u;
    Ep0[1] = (3u << 1) | (4u << 3) | (InitialMaxPacket << 16); /* CErr, control EP, MPS. */
    uint64_t Ep0Ring = XhciDeviceDmaPhysical(Device, XHCI_DEVICE_EP0_RING_PAGE) | 1u;
    Ep0[2] = (uint32_t)Ep0Ring;
    Ep0[3] = (uint32_t)(Ep0Ring >> 32);
    Ep0[4] = 8u; /* Average TRB length. */

    uint64_t OutputContext = XhciDeviceDmaPhysical(Device, XHCI_DEVICE_OUTPUT_CONTEXT_PAGE);
    Controller->Dcbaa[Device->SlotId] = OutputContext;
}

static bool XhciQueueEp0Trb(XHCI_DEVICE *Device, XHCI_TRB *Trb)
{
    volatile XHCI_TRB *Ring = (volatile XHCI_TRB *)Device->Ep0Ring;
    Trb->Control = (Trb->Control & ~XHCI_TRB_CYCLE) |
                   (Device->Ep0Cycle ? XHCI_TRB_CYCLE : 0);
    Ring[Device->Ep0Index] = *Trb;
    Device->Ep0Index++;
    if (Device->Ep0Index == XHCI_RING_TRBS - 1) {
        XHCI_TRB Link = {0};
        uint64_t Base = XhciDeviceDmaPhysical(Device, XHCI_DEVICE_EP0_RING_PAGE);
        Link.ParameterLow = (uint32_t)Base;
        Link.ParameterHigh = (uint32_t)(Base >> 32);
        Link.Control = (XHCI_TRB_LINK << XHCI_TRB_TYPE_SHIFT) | XHCI_TRB_TOGGLE_CYCLE |
                       (Device->Ep0Cycle ? XHCI_TRB_CYCLE : 0);
        Ring[Device->Ep0Index] = Link;
        Device->Ep0Index = 0;
        Device->Ep0Cycle ^= 1;
    }
    return true;
}

static bool XhciControlTransfer(XHCI_CONTROLLER *Controller, XHCI_DEVICE *Device,
                                uint8_t RequestType, uint8_t Request, uint16_t Value, uint16_t Index,
                                const void *OutputBuffer, uint16_t Length, uint32_t *ActualLength)
{
    uint8_t *Buffer = Device->Dma + XHCI_DEVICE_DESCRIPTOR_PAGE * PAGE_SIZE;
    memset(Buffer, 0, PAGE_SIZE);
    if (Length && !(RequestType & 0x80) && OutputBuffer)
        memcpy(Buffer, OutputBuffer, Length);
    XHCI_TRB Setup = {0};
    uint64_t SetupPacket = RequestType | ((uint64_t)Request << 8) | ((uint64_t)Value << 16) |
                           ((uint64_t)Index << 32) | ((uint64_t)Length << 48);
    Setup.ParameterLow = (uint32_t)SetupPacket;
    Setup.ParameterHigh = (uint32_t)(SetupPacket >> 32);
    Setup.Status = 8; /* Immediate setup packet length. */
    uint32_t TransferType = Length ? ((RequestType & 0x80) ? XHCI_TRB_TRT_IN_DATA : (2u << 16)) : 0;
    Setup.Control = (XHCI_TRB_SETUP_STAGE << XHCI_TRB_TYPE_SHIFT) | XHCI_TRB_IDT | XHCI_TRB_CHAIN | TransferType;
    XHCI_TRB Data = {0};
    uint64_t BufferPhysical = XhciDeviceDmaPhysical(Device, XHCI_DEVICE_DESCRIPTOR_PAGE);
    Data.ParameterLow = (uint32_t)BufferPhysical;
    Data.ParameterHigh = (uint32_t)(BufferPhysical >> 32);
    Data.Status = Length;
    Data.Control = (XHCI_TRB_DATA_STAGE << XHCI_TRB_TYPE_SHIFT) |
                   ((RequestType & 0x80) ? XHCI_TRB_DIR_IN : 0) | XHCI_TRB_CHAIN;
    XHCI_TRB Status = {0};
    Status.Control = (XHCI_TRB_STATUS_STAGE << XHCI_TRB_TYPE_SHIFT) | XHCI_TRB_IOC |
                     ((RequestType & 0x80) ? 0 : XHCI_TRB_DIR_IN);

    XhciQueueEp0Trb(Device, &Setup);
    if (Length)
        XhciQueueEp0Trb(Device, &Data);
    XhciQueueEp0Trb(Device, &Status);
    __asm__ volatile("" : : : "memory");
    Controller->Doorbells[Device->SlotId] = 1; /* Doorbell target 1 is endpoint 0. */

    for (uint32_t Elapsed = 0; Elapsed < XHCI_TIMEOUT_US; Elapsed += 100) {
        XHCI_TRB Event;
        if (!XhciPopEvent(Controller, &Event)) {
            KeStallExecutionProcessor(100);
            continue;
        }
        uint32_t Type = (Event.Control & XHCI_TRB_TYPE_MASK) >> XHCI_TRB_TYPE_SHIFT;
        if (Type != XHCI_TRB_TRANSFER_EVENT)
            continue;
        if ((uint8_t)(Event.Control >> XHCI_TRB_SLOT_SHIFT) != Device->SlotId)
            continue;
        uint32_t Completion = (Event.Status >> XHCI_TRB_COMPLETION_SHIFT) & 0xFF;
        uint32_t Residual = Event.Status & 0xFFFFFF;
        if (Completion != XHCI_TRB_SUCCESS && Completion != XHCI_TRB_SHORT_PACKET) {
            DbgPrintEx(DPFLTR_WARNING_LEVEL, "xHCI: EP0 completion code %u, residual %u, slot %u\n",
                       Completion, Residual, (uint8_t)(Event.Control >> XHCI_TRB_SLOT_SHIFT));
            return false;
        }
        *ActualLength = Length - Residual;
        return *ActualLength <= Length;
    }
    DbgPrintEx(DPFLTR_WARNING_LEVEL, "xHCI: EP0 transfer timed out (slot %u, USBSTS=%08x)\n",
               Device->SlotId, XhciRead32(Controller, Controller->CapabilityLength + XHCI_USBSTS));
    volatile XHCI_TRB *EventRing = (volatile XHCI_TRB *)Controller->EventRing;
    DbgPrintEx(DPFLTR_WARNING_LEVEL, "xHCI: event dequeue index=%u cycle=%u trb.control=%08x status=%08x\n",
               Controller->EventIndex, Controller->EventCycle, EventRing[Controller->EventIndex].Control,
               EventRing[Controller->EventIndex].Status);
    uint32_t ContextBytes = XhciContextBytes(Controller);
    uint32_t *Output = (uint32_t *)(Device->Dma + XHCI_DEVICE_OUTPUT_CONTEXT_PAGE * PAGE_SIZE);
    uint32_t *OutputEp0 = (uint32_t *)((uint8_t *)Output + ContextBytes);
    DbgPrintEx(DPFLTR_WARNING_LEVEL, "xHCI: output EP0 state=%u dequeue=%08x:%08x\n",
               OutputEp0[0] & 7, OutputEp0[3], OutputEp0[2]);
    return false;
}

static bool XhciControlGetDescriptor(XHCI_CONTROLLER *Controller, XHCI_DEVICE *Device,
                                     uint8_t DescriptorType, uint8_t DescriptorIndex, uint16_t Length,
                                     uint32_t *ActualLength)
{
    return XhciControlTransfer(Controller, Device, 0x80, 6,
                               (uint16_t)(((uint16_t)DescriptorType << 8) | DescriptorIndex), 0,
                               NULL, Length, ActualLength);
}

static bool XhciEnumerateDevice(XHCI_CONTROLLER *Controller, XHCI_DEVICE *Device)
{
    if (Device->Speed > 3) {
        DbgPrintEx(DPFLTR_WARNING_LEVEL, "USB: port %u speed id %u enumeration is not supported yet\n",
                   Device->Port, Device->Speed);
        return false;
    }
    uint32_t ContextBytes = XhciContextBytes(Controller);
    XhciWriteInputSlot(Controller, Device, ContextBytes);

    XHCI_TRB Command = {0};
    XHCI_TRB Completion = {0};
    uint64_t InputContext = XhciDeviceDmaPhysical(Device, XHCI_DEVICE_INPUT_CONTEXT_PAGE);
    Command.ParameterLow = (uint32_t)InputContext;
    Command.ParameterHigh = (uint32_t)(InputContext >> 32);
    Command.Control = (XHCI_TRB_ADDRESS_DEVICE << XHCI_TRB_TYPE_SHIFT) | (1u << 9) |
                      ((uint32_t)Device->SlotId << XHCI_TRB_SLOT_SHIFT); /* BSR: leave address zero. */
    if (!XhciSubmitCommand(Controller, &Command, &Completion)) {
        DbgPrintEx(DPFLTR_WARNING_LEVEL, "USB: port %u Address Device (BSR) failed\n", Device->Port);
        return false;
    }

    uint32_t ActualLength = 0;
    if (!XhciControlGetDescriptor(Controller, Device, 1, 0, 8, &ActualLength) || ActualLength < 8) {
        DbgPrintEx(DPFLTR_WARNING_LEVEL, "USB: port %u initial device descriptor request failed (%u bytes)\n",
                   Device->Port, ActualLength);
        return false;
    }
    uint8_t *Descriptor = Device->Dma + XHCI_DEVICE_DESCRIPTOR_PAGE * PAGE_SIZE;
    uint16_t MaxPacket = Descriptor[7];
    if (Device->Speed == 1 && MaxPacket != 8 && MaxPacket != 16 && MaxPacket != 32 && MaxPacket != 64)
        return false;
    if (Device->Speed == 2 && MaxPacket != 8)
        return false;
    if (Device->Speed == 3 && MaxPacket != 64)
        return false;
    Device->MaxPacket = MaxPacket;

    uint32_t *Input = XhciInputContext(Device);
    uint32_t *Ep0 = (uint32_t *)((uint8_t *)Input + 2 * ContextBytes);
    uint64_t Ep0Dequeue = (XhciDeviceDmaPhysical(Device, XHCI_DEVICE_EP0_RING_PAGE) +
                           Device->Ep0Index * sizeof(XHCI_TRB)) | Device->Ep0Cycle;
    Ep0[2] = (uint32_t)Ep0Dequeue;
    Ep0[3] = (uint32_t)(Ep0Dequeue >> 32);
    Ep0[1] = (Ep0[1] & 0xFFFFu) | ((uint32_t)MaxPacket << 16);
    Input[1] = 1u << 1; /* Evaluate Endpoint 0 Context only. */
    Command = (XHCI_TRB){0};
    Completion = (XHCI_TRB){0};
    Command.ParameterLow = (uint32_t)InputContext;
    Command.ParameterHigh = (uint32_t)(InputContext >> 32);
    Command.Control = (XHCI_TRB_EVALUATE_CONTEXT << XHCI_TRB_TYPE_SHIFT) |
                      ((uint32_t)Device->SlotId << XHCI_TRB_SLOT_SHIFT);
    if (!XhciSubmitCommand(Controller, &Command, &Completion)) {
        DbgPrintEx(DPFLTR_WARNING_LEVEL, "USB: port %u Evaluate Context failed\n", Device->Port);
        return false;
    }

    Input[1] = 1u | (1u << 1);
    Command = (XHCI_TRB){0};
    Completion = (XHCI_TRB){0};
    Command.ParameterLow = (uint32_t)InputContext;
    Command.ParameterHigh = (uint32_t)(InputContext >> 32);
    Command.Control = (XHCI_TRB_ADDRESS_DEVICE << XHCI_TRB_TYPE_SHIFT) |
                      ((uint32_t)Device->SlotId << XHCI_TRB_SLOT_SHIFT);
    if (!XhciSubmitCommand(Controller, &Command, &Completion)) {
        DbgPrintEx(DPFLTR_WARNING_LEVEL, "USB: port %u final Address Device failed\n", Device->Port);
        return false;
    }

    if (!XhciControlGetDescriptor(Controller, Device, 1, 0, 18, &ActualLength) || ActualLength < 18 ||
        Descriptor[0] < 18 || Descriptor[1] != 1) {
        DbgPrintEx(DPFLTR_WARNING_LEVEL, "USB: port %u full device descriptor invalid (%u bytes)\n",
                   Device->Port, ActualLength);
        return false;
    }
    DbgPrint("USB: device enumerated on port %u slot %u, VID=%04x PID=%04x class=%02x\n",
             Device->Port, Device->SlotId, (uint32_t)(Descriptor[8] | ((uint16_t)Descriptor[9] << 8)),
             (uint32_t)(Descriptor[10] | ((uint16_t)Descriptor[11] << 8)), Descriptor[4]);
    return true;
}

static uint8_t XhciInterruptInterval(uint8_t Speed, uint8_t UsbInterval)
{
    if (!UsbInterval)
        return 1;
    if (Speed >= 3)
        return UsbInterval - 1;
    uint8_t Log = 0;
    uint32_t Frames = UsbInterval - 1;
    while (Frames) {
        Frames >>= 1;
        Log++;
    }
    return (uint8_t)(3 + Log);
}

static bool XhciConfigureHidEndpoint(XHCI_CONTROLLER *Controller, XHCI_DEVICE *Device,
                                     uint8_t InterfaceNumber, uint8_t Protocol, uint8_t EndpointAddress,
                                     uint16_t MaxPacket, uint8_t Interval)
{
    if (!(EndpointAddress & 0x80) || !(EndpointAddress & 0x0F) || !MaxPacket || MaxPacket > 1024)
        return false;
    uint8_t Dci = (uint8_t)(((EndpointAddress & 0x0F) * 2) + 1);
    if (Dci > 31)
        return false;

    Device->InterfaceNumber = InterfaceNumber;
    Device->EndpointAddress = EndpointAddress;
    Device->EndpointDci = Dci;
    Device->MaxPacket = MaxPacket;
    Device->Interval = Interval;
    Device->ReportBytes = Protocol == 1 ? 8 : 3;
    Device->IsKeyboard = Protocol == 1;
    Device->IsMouse = Protocol == 2;
    Device->InterruptRing = (volatile uint32_t *)(Device->Dma + XHCI_DEVICE_INTERRUPT_RING_PAGE * PAGE_SIZE);
    Device->InterruptIndex = 0;
    Device->InterruptCycle = 1;

    volatile XHCI_TRB *Ring = (volatile XHCI_TRB *)Device->InterruptRing;
    XHCI_TRB Link = {0};
    uint64_t RingBase = XhciDeviceDmaPhysical(Device, XHCI_DEVICE_INTERRUPT_RING_PAGE);
    Link.ParameterLow = (uint32_t)RingBase;
    Link.ParameterHigh = (uint32_t)(RingBase >> 32);
    Link.Control = (XHCI_TRB_LINK << XHCI_TRB_TYPE_SHIFT) | XHCI_TRB_TOGGLE_CYCLE | XHCI_TRB_CYCLE;
    Ring[XHCI_RING_TRBS - 1] = Link;

    uint32_t ContextBytes = XhciContextBytes(Controller);
    uint32_t *Input = XhciInputContext(Device);
    uint32_t *InputSlot = (uint32_t *)((uint8_t *)Input + ContextBytes);
    uint32_t *InputEndpoint = (uint32_t *)((uint8_t *)Input + (Dci + 1) * ContextBytes);
    uint32_t *OutputSlot = (uint32_t *)(Device->Dma + XHCI_DEVICE_OUTPUT_CONTEXT_PAGE * PAGE_SIZE);
    memset(Input, 0, PAGE_SIZE);
    memcpy(InputSlot, OutputSlot, ContextBytes);
    InputSlot[0] = (InputSlot[0] & ~(0x1Fu << 27)) | ((uint32_t)Dci << 27);
    Input[1] = 1u | (1u << Dci); /* Add the slot and this endpoint context (flag bit = DCI). */

    uint8_t IntervalContext = XhciInterruptInterval(Device->Speed, Interval);
    InputEndpoint[0] = (uint32_t)IntervalContext << 16;
    InputEndpoint[1] = (3u << 1) | (7u << 3) | ((uint32_t)MaxPacket << 16); /* Interrupt IN. */
    uint64_t Dequeue = RingBase | 1u;
    InputEndpoint[2] = (uint32_t)Dequeue;
    InputEndpoint[3] = (uint32_t)(Dequeue >> 32);
    InputEndpoint[4] = Device->ReportBytes;

    uint64_t InputPhysical = XhciDeviceDmaPhysical(Device, XHCI_DEVICE_INPUT_CONTEXT_PAGE);
    XHCI_TRB Command = {0};
    XHCI_TRB Completion = {0};
    Command.ParameterLow = (uint32_t)InputPhysical;
    Command.ParameterHigh = (uint32_t)(InputPhysical >> 32);
    Command.Control = (XHCI_TRB_CONFIGURE_ENDPOINT << XHCI_TRB_TYPE_SHIFT) |
                      ((uint32_t)Device->SlotId << XHCI_TRB_SLOT_SHIFT);
    if (!XhciSubmitCommand(Controller, &Command, &Completion))
        return false;

    uint8_t *Report = Device->Dma + XHCI_DEVICE_REPORT_PAGE * PAGE_SIZE;
    memset(Report, 0, PAGE_SIZE);
    XHCI_TRB Transfer = {0};
    uint64_t ReportPhysical = XhciDeviceDmaPhysical(Device, XHCI_DEVICE_REPORT_PAGE);
    Transfer.ParameterLow = (uint32_t)ReportPhysical;
    Transfer.ParameterHigh = (uint32_t)(ReportPhysical >> 32);
    Transfer.Status = Device->ReportBytes;
    Transfer.Control = (XHCI_TRB_NORMAL << XHCI_TRB_TYPE_SHIFT) | XHCI_TRB_IOC | XHCI_TRB_CYCLE;
    Ring[0] = Transfer;
    Device->InterruptIndex = 1;
    __asm__ volatile("" : : : "memory");
    Controller->Doorbells[Device->SlotId] = Dci;
    DbgPrint("USB HID: %s ready, port %u slot %u endpoint %02x (%u-byte reports)\n",
             Device->IsKeyboard ? "boot keyboard" : "boot mouse", Device->Port, Device->SlotId,
             EndpointAddress, Device->ReportBytes);
    return true;
}

static bool XhciInitializeHid(XHCI_CONTROLLER *Controller, XHCI_DEVICE *Device)
{
    uint32_t ActualLength = 0;
    if (!XhciControlGetDescriptor(Controller, Device, 2, 0, 9, &ActualLength) || ActualLength < 9)
        return false;
    uint8_t *Config = Device->Dma + XHCI_DEVICE_DESCRIPTOR_PAGE * PAGE_SIZE;
    if (Config[1] != 2)
        return false;
    uint16_t TotalLength = (uint16_t)(Config[2] | ((uint16_t)Config[3] << 8));
    if (TotalLength < 9 || TotalLength > PAGE_SIZE ||
        !XhciControlGetDescriptor(Controller, Device, 2, 0, TotalLength, &ActualLength) ||
        ActualLength < TotalLength)
        return false;
    memcpy(Device->Dma + XHCI_DEVICE_CONFIG_PAGE * PAGE_SIZE, Config, TotalLength);
    Config = Device->Dma + XHCI_DEVICE_CONFIG_PAGE * PAGE_SIZE;

    bool InBootInterface = false;
    bool FoundEndpoint = false;
    uint8_t InterfaceNumber = 0, Protocol = 0, EndpointAddress = 0, Interval = 0;
    uint16_t MaxPacket = 0;
    for (uint32_t Offset = 0; Offset + 2 <= TotalLength;) {
        uint8_t Length = Config[Offset];
        uint8_t Type = Config[Offset + 1];
        if (Length < 2 || Offset + Length > TotalLength)
            return false;
        if (Type == 4 && Length >= 9) {
            InBootInterface = Config[Offset + 5] == 3 && Config[Offset + 6] == 1 &&
                              (Config[Offset + 7] == 1 || Config[Offset + 7] == 2);
            if (InBootInterface) {
                InterfaceNumber = Config[Offset + 2];
                Protocol = Config[Offset + 7];
            }
        } else if (Type == 5 && Length >= 7 && InBootInterface &&
                   (Config[Offset + 3] & 3u) == 3u && (Config[Offset + 2] & 0x80)) {
            EndpointAddress = Config[Offset + 2];
            MaxPacket = (uint16_t)(Config[Offset + 4] | ((uint16_t)Config[Offset + 5] << 8)) & 0x07FF;
            Interval = Config[Offset + 6];
            FoundEndpoint = true;
            break;
        }
        Offset += Length;
    }
    if (!FoundEndpoint) {
        DbgPrint("USB: port %u enumerated; no boot HID keyboard/mouse interface\n", Device->Port);
        return true;
    }

    Device->Configuration = Config[5];
    uint32_t Actual = 0;
    if (!XhciControlTransfer(Controller, Device, 0x00, 9, Device->Configuration, 0, NULL, 0, &Actual) ||
        !XhciControlTransfer(Controller, Device, 0x21, 0x0B, 0, InterfaceNumber, NULL, 0, &Actual))
        return false;
    /* Set Idle is optional for a boot keyboard; tolerate devices that stall it. */
    (void)XhciControlTransfer(Controller, Device, 0x21, 0x0A, 0, InterfaceNumber, NULL, 0, &Actual);
    return XhciConfigureHidEndpoint(Controller, Device, InterfaceNumber, Protocol, EndpointAddress,
                                    MaxPacket, Interval);
}

/* One outstanding bulk TRB at a time. BOT is deliberately serialized per device. */
static bool XhciBulkTransfer(XHCI_CONTROLLER *Controller, XHCI_DEVICE *Device, bool In,
                            void *Buffer, uint32_t Length)
{
    uint8_t Dci = In ? Device->BulkInDci : Device->BulkOutDci;
    uint8_t *Cycle = In ? &Device->BulkInCycle : &Device->BulkOutCycle;
    uint32_t *Index = In ? &Device->BulkInIndex : &Device->BulkOutIndex;
    uint32_t Page = In ? XHCI_DEVICE_BULK_IN_RING_PAGE : XHCI_DEVICE_BULK_OUT_RING_PAGE;
    volatile XHCI_TRB *Ring = (volatile XHCI_TRB *)(Device->Dma + Page * PAGE_SIZE);
    uint64_t Physical = XhciDeviceDmaPhysical(Device, XHCI_DEVICE_BULK_BUFFER_PAGE);
    if (Length > PAGE_SIZE)
        return false;
    KeWaitForSingleObject(&Controller->EventLock, NULL);
    if (!In && Length)
        memcpy(Device->Dma + XHCI_DEVICE_BULK_BUFFER_PAGE * PAGE_SIZE, Buffer, Length);
    XHCI_TRB Trb = {0};
    Trb.ParameterLow = (uint32_t)Physical;
    Trb.ParameterHigh = (uint32_t)(Physical >> 32);
    Trb.Status = Length;
    Trb.Control = (XHCI_TRB_NORMAL << XHCI_TRB_TYPE_SHIFT) | XHCI_TRB_IOC |
                  (*Cycle ? XHCI_TRB_CYCLE : 0);
    Ring[*Index] = Trb;
    (*Index)++;
    if (*Index == XHCI_RING_TRBS - 1) {
        XHCI_TRB Link = {0};
        uint64_t RingPhysical = XhciDeviceDmaPhysical(Device, Page);
        Link.ParameterLow = (uint32_t)RingPhysical;
        Link.ParameterHigh = (uint32_t)(RingPhysical >> 32);
        Link.Control = (XHCI_TRB_LINK << XHCI_TRB_TYPE_SHIFT) | XHCI_TRB_TOGGLE_CYCLE |
                       (*Cycle ? XHCI_TRB_CYCLE : 0);
        Ring[*Index] = Link;
        *Index = 0;
        *Cycle ^= 1;
    }
    __asm__ volatile("" : : : "memory");
    Controller->Doorbells[Device->SlotId] = Dci;
    bool Completed = false;
    for (uint32_t Elapsed = 0; Elapsed < XHCI_TIMEOUT_US; Elapsed += 100) {
        XHCI_TRB Event;
        if (!XhciPopEvent(Controller, &Event)) {
            KeStallExecutionProcessor(100);
            continue;
        }
        if (((Event.Control & XHCI_TRB_TYPE_MASK) >> XHCI_TRB_TYPE_SHIFT) != XHCI_TRB_TRANSFER_EVENT ||
            (uint8_t)(Event.Control >> XHCI_TRB_SLOT_SHIFT) != Device->SlotId ||
            ((Event.Control >> 16) & 0x1Fu) != Dci)
            continue;
        uint32_t Code = (Event.Status >> XHCI_TRB_COMPLETION_SHIFT) & 0xFF;
        uint32_t Residual = Event.Status & 0xFFFFFF;
        Completed = (Code == XHCI_TRB_SUCCESS || Code == XHCI_TRB_SHORT_PACKET) && Residual <= Length;
        if (!Completed)
            DbgPrintEx(DPFLTR_WARNING_LEVEL, "USB MSC: bulk %s DCI %u completion %u residual %u/%u\n",
                       In ? "IN" : "OUT", Dci, Code, Residual, Length);
        break;
    }
    if (In && Completed && Length)
        memcpy(Buffer, Device->Dma + XHCI_DEVICE_BULK_BUFFER_PAGE * PAGE_SIZE, Length);
    if (!Completed)
        DbgPrintEx(DPFLTR_WARNING_LEVEL, "USB MSC: bulk %s DCI %u timed out\n", In ? "IN" : "OUT", Dci);
    KeReleaseMutex(&Controller->EventLock);
    return Completed;
}

typedef struct HZ_PACKED _USB_MSC_CBW {
    uint32_t Signature;
    uint32_t Tag;
    uint32_t TransferLength;
    uint8_t Flags;
    uint8_t Lun;
    uint8_t CommandLength;
    uint8_t Command[16];
} USB_MSC_CBW;

typedef struct HZ_PACKED _USB_MSC_CSW {
    uint32_t Signature;
    uint32_t Tag;
    uint32_t Residue;
    uint8_t Status;
} USB_MSC_CSW;

static bool XhciMscCommand(XHCI_DEVICE *Device, const uint8_t *Cdb, uint8_t CdbLength,
                           bool In, void *Data, uint32_t DataLength)
{
    XHCI_CONTROLLER *Controller = Device->Controller;
    uint8_t *Buffer = Device->Dma + XHCI_DEVICE_BULK_BUFFER_PAGE * PAGE_SIZE;
    USB_MSC_CBW Cbw;
    memset(&Cbw, 0, sizeof(Cbw));
    Cbw.Signature = 0x43425355u;
    Cbw.Tag = 0x4E584F52u;
    Cbw.TransferLength = DataLength;
    Cbw.Flags = In ? 0x80 : 0;
    Cbw.Lun = (uint8_t)Device->MaxLun;
    Cbw.CommandLength = CdbLength;
    memcpy(Cbw.Command, Cdb, CdbLength);
    if (!XhciBulkTransfer(Controller, Device, false, &Cbw, sizeof(Cbw))) {
        DbgPrintEx(DPFLTR_WARNING_LEVEL, "USB MSC: CBW transfer failed\n");
        return false;
    }
    if (DataLength && !XhciBulkTransfer(Controller, Device, In, Data, DataLength)) {
        DbgPrintEx(DPFLTR_WARNING_LEVEL, "USB MSC: data phase failed (CDB %02x)\n", Cdb[0]);
        return false;
    }
    USB_MSC_CSW Csw;
    memset(&Csw, 0, sizeof(Csw));
    if (!XhciBulkTransfer(Controller, Device, true, &Csw, sizeof(Csw))) {
        DbgPrintEx(DPFLTR_WARNING_LEVEL, "USB MSC: CSW transfer failed (CDB %02x)\n", Cdb[0]);
        return false;
    }
    memcpy(&Csw, Buffer, sizeof(Csw));
    bool Good = Csw.Signature == 0x53425355u && Csw.Tag == Cbw.Tag && Csw.Status == 0;
    if (!Good)
        DbgPrintEx(DPFLTR_WARNING_LEVEL, "USB MSC: invalid CSW signature=%08x tag=%08x status=%u\n",
                   Csw.Signature, Csw.Tag, Csw.Status);
    return Good;
}

static STATUS XhciMscTransfer(void *Context, uint64_t Lba, uint32_t Count, void *Buffer, bool Write)
{
    XHCI_DEVICE *Device = Context;
    if (!Count || Lba > 0xFFFFFFFFull || Count > 8 || Lba + Count > Device->SectorCount)
        return STATUS_INVALID_PARAMETER;
    uint8_t Cdb[10] = {Write ? 0x2A : 0x28, 0};
    Cdb[2] = (uint8_t)(Lba >> 24); Cdb[3] = (uint8_t)(Lba >> 16);
    Cdb[4] = (uint8_t)(Lba >> 8); Cdb[5] = (uint8_t)Lba;
    Cdb[7] = (uint8_t)(Count >> 8); Cdb[8] = (uint8_t)Count;
    return XhciMscCommand(Device, Cdb, sizeof(Cdb), !Write, Buffer, Count * 512) ?
           STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
}

static bool XhciConfigureMscEndpoints(XHCI_CONTROLLER *Controller, XHCI_DEVICE *Device,
                                      uint8_t Configuration, uint8_t BulkInAddress,
                                      uint16_t BulkInPacket, uint8_t BulkOutAddress,
                                      uint16_t BulkOutPacket)
{
    uint8_t InDci = (uint8_t)(((BulkInAddress & 0x0F) * 2) + 1);
    uint8_t OutDci = (uint8_t)((BulkOutAddress & 0x0F) * 2);
    if (!(BulkInAddress & 0x80) || (BulkOutAddress & 0x80) || !InDci || !OutDci ||
        InDci > 31 || OutDci > 31 || !BulkInPacket || !BulkOutPacket)
        return false;
    uint32_t ContextBytes = XhciContextBytes(Controller);
    uint32_t *Input = XhciInputContext(Device);
    uint32_t *Output = (uint32_t *)(Device->Dma + XHCI_DEVICE_OUTPUT_CONTEXT_PAGE * PAGE_SIZE);
    uint32_t *InputSlot = (uint32_t *)((uint8_t *)Input + ContextBytes);
    memset(Input, 0, PAGE_SIZE);
    memcpy(InputSlot, Output, ContextBytes);
    uint8_t Entries = InDci > OutDci ? InDci : OutDci;
    InputSlot[0] = (InputSlot[0] & ~(0x1Fu << 27)) | ((uint32_t)Entries << 27);
    Input[1] = 1u | (1u << InDci) | (1u << OutDci);
    const uint8_t Dcis[2] = {OutDci, InDci};
    const uint8_t Pages[2] = {XHCI_DEVICE_BULK_OUT_RING_PAGE, XHCI_DEVICE_BULK_IN_RING_PAGE};
    const uint16_t Packets[2] = {BulkOutPacket, BulkInPacket};
    for (uint32_t i = 0; i < 2; i++) {
        uint8_t Dci = Dcis[i];
        uint32_t *Ep = (uint32_t *)((uint8_t *)Input + (Dci + 1) * ContextBytes);
        uint64_t RingPhysical = XhciDeviceDmaPhysical(Device, Pages[i]);
        volatile XHCI_TRB *Ring = (volatile XHCI_TRB *)(Device->Dma + Pages[i] * PAGE_SIZE);
        XHCI_TRB Link = {0};
        Link.ParameterLow = (uint32_t)RingPhysical; Link.ParameterHigh = (uint32_t)(RingPhysical >> 32);
        Link.Control = (XHCI_TRB_LINK << XHCI_TRB_TYPE_SHIFT) | XHCI_TRB_TOGGLE_CYCLE | XHCI_TRB_CYCLE;
        Ring[XHCI_RING_TRBS - 1] = Link;
        Ep[1] = (3u << 1) | ((Dci == InDci ? 6u : 2u) << 3) | ((uint32_t)Packets[i] << 16);
        uint64_t Dequeue = RingPhysical | 1u;
        Ep[2] = (uint32_t)Dequeue; Ep[3] = (uint32_t)(Dequeue >> 32); Ep[4] = 512;
    }
    uint64_t Physical = XhciDeviceDmaPhysical(Device, XHCI_DEVICE_INPUT_CONTEXT_PAGE);
    XHCI_TRB Command = {0}, Completion = {0};
    Command.ParameterLow = (uint32_t)Physical; Command.ParameterHigh = (uint32_t)(Physical >> 32);
    Command.Control = (XHCI_TRB_CONFIGURE_ENDPOINT << XHCI_TRB_TYPE_SHIFT) |
                      ((uint32_t)Device->SlotId << XHCI_TRB_SLOT_SHIFT);
    if (!XhciSubmitCommand(Controller, &Command, &Completion)) {
        DbgPrintEx(DPFLTR_WARNING_LEVEL, "USB MSC: Configure Endpoint failed (IN DCI %u OUT DCI %u)\n",
                   InDci, OutDci);
        return false;
    }
    Device->Configuration = Configuration;
    Device->BulkInDci = InDci; Device->BulkOutDci = OutDci;
    Device->BulkInPacket = BulkInPacket; Device->BulkOutPacket = BulkOutPacket;
    Device->BulkInCycle = Device->BulkOutCycle = 1;
    Device->BulkInIndex = Device->BulkOutIndex = 0;
    Device->IsMassStorage = true;
    return true;
}

static bool XhciInitializeMassStorage(XHCI_CONTROLLER *Controller, XHCI_DEVICE *Device)
{
    uint32_t Actual = 0;
    if (!XhciControlGetDescriptor(Controller, Device, 2, 0, 9, &Actual) || Actual < 9)
        return false;
    uint8_t *Config = Device->Dma + XHCI_DEVICE_DESCRIPTOR_PAGE * PAGE_SIZE;
    uint16_t Total = (uint16_t)(Config[2] | ((uint16_t)Config[3] << 8));
    if (Total < 9 || Total > PAGE_SIZE || !XhciControlGetDescriptor(Controller, Device, 2, 0, Total, &Actual) || Actual < Total)
        return false;
    memcpy(Device->Dma + XHCI_DEVICE_CONFIG_PAGE * PAGE_SIZE, Config, Total);
    Config = Device->Dma + XHCI_DEVICE_CONFIG_PAGE * PAGE_SIZE;
    bool Msc = false;
    uint8_t Interface = 0, Configuration = Config[5], InAddr = 0, OutAddr = 0;
    uint16_t InPacket = 0, OutPacket = 0;
    for (uint32_t Offset = 0; Offset + 2 <= Total;) {
        uint8_t Length = Config[Offset], Type = Config[Offset + 1];
        if (Length < 2 || Offset + Length > Total) return false;
        if (Type == 4 && Length >= 9)
            Msc = Config[Offset + 5] == 8 && Config[Offset + 6] == 6 && Config[Offset + 7] == 0x50,
            Interface = Msc ? Config[Offset + 2] : Interface;
        else if (Type == 5 && Length >= 7 && Msc && (Config[Offset + 3] & 3u) == 2u) {
            uint8_t Address = Config[Offset + 2];
            uint16_t Packet = (uint16_t)(Config[Offset + 4] | ((uint16_t)Config[Offset + 5] << 8)) & 0x07FF;
            if (Address & 0x80) { InAddr = Address; InPacket = Packet; }
            else { OutAddr = Address; OutPacket = Packet; }
        }
        Offset += Length;
    }
    if (!InAddr || !OutAddr ||
        !XhciControlTransfer(Controller, Device, 0, 9, Configuration, 0, NULL, 0, &Actual) ||
        !XhciConfigureMscEndpoints(Controller, Device, Configuration,
                                   InAddr, InPacket, OutAddr, OutPacket)) {
        DbgPrintEx(DPFLTR_WARNING_LEVEL, "USB MSC: interface/endpoint configuration failed\n");
        return false;
    }
    Device->Controller = Controller;
    uint8_t Cdb[12] = {0x12, 0, 0, 0, 36, 0}, Inquiry[36] = {0};
    if (!XhciMscCommand(Device, Cdb, 6, true, Inquiry, sizeof(Inquiry))) {
        DbgPrintEx(DPFLTR_WARNING_LEVEL, "USB MSC: SCSI INQUIRY failed\n"); return false;
    }
    uint8_t CapacityCdb[10] = {0x25}, Capacity[8] = {0};
    if (!XhciMscCommand(Device, CapacityCdb, 10, true, Capacity, sizeof(Capacity))) {
        DbgPrintEx(DPFLTR_WARNING_LEVEL, "USB MSC: READ CAPACITY failed\n"); return false;
    }
    uint32_t LastLba = ((uint32_t)Capacity[0] << 24) | ((uint32_t)Capacity[1] << 16) |
                       ((uint32_t)Capacity[2] << 8) | Capacity[3];
    uint32_t SectorSize = ((uint32_t)Capacity[4] << 24) | ((uint32_t)Capacity[5] << 16) |
                         ((uint32_t)Capacity[6] << 8) | Capacity[7];
    if (SectorSize != 512) return false;
    Device->SectorCount = (uint64_t)LastLba + 1;
    if (Device->SectorCount > 0x100000000ull)
        Device->SectorCount = 0x100000000ull; /* READ/WRITE(10) address range. */
    DISK_DESCRIPTION Disk; memset(&Disk, 0, sizeof(Disk));
    RtlStringCopy(Disk.Model, sizeof(Disk.Model), "USB Mass Storage (BOT)");
    Disk.SectorSize = SectorSize; Disk.SectorCount = Device->SectorCount;
    Disk.MaxTransferSectors = PAGE_SIZE / SectorSize; Disk.Transfer = XhciMscTransfer; Disk.Context = Device;
    uint32_t DiskNumber = 0;
    STATUS Status = DiskCreateDisk(&Disk, &DiskNumber);
    if (!HZ_SUCCESS(Status)) return false;
    DbgPrint("USB MSC: BOT disk %u online, %llu sectors, read/write\n", DiskNumber,
             (unsigned long long)Device->SectorCount);
    return true;
}

static uint8_t XhciHidVirtualKey(uint8_t Usage)
{
    if (Usage >= 4 && Usage <= 29)
        return (uint8_t)('A' + Usage - 4);
    if (Usage >= 0x1E && Usage <= 0x27)
        return Usage == 0x27 ? '0' : (uint8_t)('1' + Usage - 0x1E);
    switch (Usage) {
    case 0x28: return HZ_VK_RETURN;
    case 0x29: return HZ_VK_ESCAPE;
    case 0x2A: return HZ_VK_BACK;
    case 0x2B: return HZ_VK_TAB;
    case 0x2C: return HZ_VK_SPACE;
    case 0x39: return HZ_VK_CAPITAL;
    case 0x46: return HZ_VK_SCROLL;
    case 0x53: return HZ_VK_NUMLOCK;
    case 0x49: return HZ_VK_INSERT;
    case 0x4A: return HZ_VK_HOME;
    case 0x4B: return HZ_VK_PRIOR;
    case 0x4C: return HZ_VK_DELETE;
    case 0x4D: return HZ_VK_END;
    case 0x4E: return HZ_VK_NEXT;
    case 0x4F: return HZ_VK_RIGHT;
    case 0x50: return HZ_VK_LEFT;
    case 0x51: return HZ_VK_DOWN;
    case 0x52: return HZ_VK_UP;
    default:
        if (Usage >= 0x3A && Usage <= 0x45)
            return (uint8_t)(HZ_VK_F1 + Usage - 0x3A);
        return HZ_VK_OEM;
    }
}

static uint8_t XhciHidScanCode(uint8_t Usage)
{
    static const uint8_t LetterScans[26] = {
        0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26, 0x32,
        0x31, 0x18, 0x19, 0x10, 0x13, 0x1F, 0x14, 0x16, 0x2F, 0x11, 0x2D, 0x15, 0x2C};
    if (Usage >= 4 && Usage <= 29)
        return LetterScans[Usage - 4];
    if (Usage >= 0x1E && Usage <= 0x26)
        return (uint8_t)(2 + Usage - 0x1E);
    if (Usage == 0x27)
        return 0x0B;
    switch (Usage) {
    case 0x28: return 0x1C;
    case 0x29: return 0x01;
    case 0x2A: return 0x0E;
    case 0x2B: return 0x0F;
    case 0x2C: return 0x39;
    default: return Usage;
    }
}

static uint8_t XhciHidCharacter(uint8_t Usage, bool Shift, bool Caps)
{
    if (Usage >= 4 && Usage <= 29) {
        bool Upper = Shift != Caps;
        return (uint8_t)((Upper ? 'A' : 'a') + Usage - 4);
    }
    static const char Digits[] = "1234567890";
    static const char ShiftedDigits[] = "!@#$%^&*()";
    if (Usage >= 0x1E && Usage <= 0x27)
        return (uint8_t)(Shift ? ShiftedDigits[Usage - 0x1E] : Digits[Usage - 0x1E]);
    if (Usage == 0x2C) return ' ';
    if (Usage == 0x28) return '\r';
    if (Usage == 0x2B) return '\t';
    static const uint8_t Plain[] = {'-', '=', '[', ']', '\\', 0, ';', '\'', '`', ',', '.', '/'};
    static const uint8_t Shifted[] = {'_', '+', '{', '}', '|', 0, ':', '"', '~', '<', '>', '?'};
    if (Usage >= 0x2D && Usage <= 0x38)
        return Shift ? Shifted[Usage - 0x2D] : Plain[Usage - 0x2D];
    return 0;
}

static void XhciEmitHidKey(XHCI_DEVICE *Device, uint8_t Usage, bool Break)
{
    HZ_KEY_EVENT Event;
    memset(&Event, 0, sizeof(Event));
    Event.TimeUs = KeQueryUptimeMicroseconds();
    Event.ScanCode = XhciHidScanCode(Usage);
    Event.Flags = Break ? HZ_KEY_BREAK : 0;
    Event.Modifiers = Device->Modifiers;
    Event.VirtualKey = XhciHidVirtualKey(Usage);
    Event.Character = Break ? 0 : XhciHidCharacter(Usage,
        (Device->Modifiers & HZ_MOD_SHIFT) != 0, (Device->Modifiers & HZ_MOD_CAPS_LOCK) != 0);
    InputSubmitKeyEvent(&Event);
    if (!Break)
        DbgPrint("USB HID: key %u down (vk=%02x)\n", Usage, Event.VirtualKey);
}

static bool XhciReportHasKey(const uint8_t Report[8], uint8_t Usage)
{
    for (uint32_t i = 2; i < 8; i++) {
        if (Report[i] == Usage)
            return true;
    }
    return false;
}

static void XhciProcessKeyboardReport(XHCI_DEVICE *Device, const uint8_t Report[8])
{
    uint8_t PreviousModifiers = Device->PreviousReport[0];
    uint8_t CurrentModifiers = Report[0];
    uint16_t Locks = Device->LockModifiers;
    Device->LockModifiers = (uint8_t)Locks;
    Device->Modifiers = (uint8_t)(Locks |
        ((CurrentModifiers & ((1u << 0) | (1u << 4))) ? HZ_MOD_CTRL : 0) |
        ((CurrentModifiers & ((1u << 1) | (1u << 5))) ? HZ_MOD_SHIFT : 0) |
        ((CurrentModifiers & ((1u << 2) | (1u << 6))) ? HZ_MOD_ALT : 0));

    static const uint8_t ModifierUsage[8] = {0xE0, 0xE1, 0xE2, 0xE3, 0xE4, 0xE5, 0xE6, 0xE7};
    for (uint32_t Bit = 0; Bit < 8; Bit++) {
        bool WasDown = (PreviousModifiers & (1u << Bit)) != 0;
        bool IsDown = (CurrentModifiers & (1u << Bit)) != 0;
        if (WasDown != IsDown) {
            uint8_t VKey = (Bit == 0 || Bit == 4) ? HZ_VK_CONTROL :
                           (Bit == 1 || Bit == 5) ? HZ_VK_SHIFT :
                           (Bit == 2 || Bit == 6) ? HZ_VK_MENU : HZ_VK_LWIN;
            HZ_KEY_EVENT Event = {0};
            Event.TimeUs = KeQueryUptimeMicroseconds();
            Event.ScanCode = XhciHidScanCode(ModifierUsage[Bit]);
            Event.Flags = IsDown ? 0 : HZ_KEY_BREAK;
            Event.Modifiers = Device->Modifiers;
            Event.VirtualKey = VKey;
            InputSubmitKeyEvent(&Event);
        }
    }

    bool Rollover = false;
    for (uint32_t i = 2; i < 8; i++)
        Rollover |= Report[i] == 1;
    if (!Rollover) {
        for (uint32_t i = 2; i < 8; i++) {
            uint8_t Usage = Device->PreviousReport[i];
            if (Usage >= 4 && !XhciReportHasKey(Report, Usage))
                XhciEmitHidKey(Device, Usage, true);
        }
        for (uint32_t i = 2; i < 8; i++) {
            uint8_t Usage = Report[i];
            if (Usage >= 4 && !XhciReportHasKey(Device->PreviousReport, Usage)) {
                if (Usage == 0x39)
                    Device->LockModifiers ^= HZ_MOD_CAPS_LOCK;
                else if (Usage == 0x53)
                    Device->LockModifiers ^= HZ_MOD_NUM_LOCK;
                Device->Modifiers = (uint8_t)(Device->LockModifiers |
                    ((CurrentModifiers & ((1u << 0) | (1u << 4))) ? HZ_MOD_CTRL : 0) |
                    ((CurrentModifiers & ((1u << 1) | (1u << 5))) ? HZ_MOD_SHIFT : 0) |
                    ((CurrentModifiers & ((1u << 2) | (1u << 6))) ? HZ_MOD_ALT : 0));
                XhciEmitHidKey(Device, Usage, false);
            }
        }
    }
    memcpy(Device->PreviousReport, Report, 8);
}

static bool XhciQueueInterruptReport(XHCI_CONTROLLER *Controller, XHCI_DEVICE *Device)
{
    volatile XHCI_TRB *Ring = (volatile XHCI_TRB *)Device->InterruptRing;
    uint64_t ReportPhysical = XhciDeviceDmaPhysical(Device, XHCI_DEVICE_REPORT_PAGE);
    XHCI_TRB Trb = {0};
    Trb.ParameterLow = (uint32_t)ReportPhysical;
    Trb.ParameterHigh = (uint32_t)(ReportPhysical >> 32);
    Trb.Status = Device->ReportBytes;
    Trb.Control = (XHCI_TRB_NORMAL << XHCI_TRB_TYPE_SHIFT) | XHCI_TRB_IOC |
                  (Device->InterruptCycle ? XHCI_TRB_CYCLE : 0);
    Ring[Device->InterruptIndex] = Trb;
    Device->InterruptIndex++;
    if (Device->InterruptIndex == XHCI_RING_TRBS - 1) {
        Device->InterruptIndex = 0;
        Device->InterruptCycle ^= 1;
    }
    __asm__ volatile("" : : : "memory");
    Controller->Doorbells[Device->SlotId] = Device->EndpointDci;
    return true;
}

static void XhciInputThread(void *Context)
{
    XHCI_CONTROLLER *Controller = Context;
    DbgPrint("USB HID: input polling thread active\n");
    for (;;) {
        XHCI_TRB Event;
        KeWaitForSingleObject(&Controller->EventLock, NULL);
        bool HaveEvent = XhciPopEvent(Controller, &Event);
        KeReleaseMutex(&Controller->EventLock);
        if (!HaveEvent) {
            KeStallExecutionProcessor(1000);
            continue;
        }
        uint32_t Type = (Event.Control & XHCI_TRB_TYPE_MASK) >> XHCI_TRB_TYPE_SHIFT;
        if (Type != XHCI_TRB_TRANSFER_EVENT)
            continue;
        uint8_t SlotId = (uint8_t)(Event.Control >> XHCI_TRB_SLOT_SHIFT);
        uint8_t Dci = (uint8_t)((Event.Control >> 16) & 0x1F);
        if (!SlotId || SlotId > XHCI_MAX_ACTIVE_DEVICES)
            continue;
        XHCI_DEVICE *Device = &Controller->Devices[SlotId - 1];
        if (!Device->Active || (!Device->IsKeyboard && !Device->IsMouse) || Device->EndpointDci != Dci)
            continue;
        uint32_t Completion = (Event.Status >> XHCI_TRB_COMPLETION_SHIFT) & 0xFF;
        if (Completion != XHCI_TRB_SUCCESS && Completion != XHCI_TRB_SHORT_PACKET) {
            DbgPrintEx(DPFLTR_WARNING_LEVEL, "USB HID: slot %u transfer completion %u\n", SlotId, Completion);
            continue;
        }
        DbgPrint("USB HID: transfer event slot %u endpoint %u completion %u\n", SlotId, Dci, Completion);
        uint8_t Report[8] = {0};
        memcpy(Report, Device->Dma + XHCI_DEVICE_REPORT_PAGE * PAGE_SIZE,
               HZ_MIN(Device->ReportBytes, sizeof(Report)));
        if (Device->IsKeyboard)
            XhciProcessKeyboardReport(Device, Report);
        else if (Device->IsMouse && Device->ReportBytes >= 3) {
            HZ_MOUSE_EVENT Mouse = {0};
            Mouse.TimeUs = KeQueryUptimeMicroseconds();
            Mouse.Buttons = Report[0] & (HZ_MOUSE_LEFT | HZ_MOUSE_RIGHT | HZ_MOUSE_MIDDLE);
            Mouse.DeltaX = (int8_t)Report[1];
            Mouse.DeltaY = (int8_t)Report[2];
            if (Device->ReportBytes >= 4)
                Mouse.Wheel = -(int8_t)Report[3];
            InputSubmitMouseEvent(&Mouse);
            static uint32_t MouseReportLogCount;
            if (MouseReportLogCount++ < 8)
                DbgPrint("USB HID: mouse report buttons=%02x dx=%d dy=%d wheel=%d\n", Mouse.Buttons,
                         Mouse.DeltaX, Mouse.DeltaY, Mouse.Wheel);
        }
        memset(Device->Dma + XHCI_DEVICE_REPORT_PAGE * PAGE_SIZE, 0, PAGE_SIZE);
        XhciQueueInterruptReport(Controller, Device);
    }
}

static bool XhciResetPort(XHCI_CONTROLLER *Controller, uint32_t Port, uint32_t *Speed)
{
    uint32_t PortOffset = Controller->CapabilityLength + XHCI_OP_PORTS + (Port - 1) * 0x10;
    volatile uint32_t *PortStatus = (volatile uint32_t *)(Controller->Registers + PortOffset);
    uint32_t Status = *PortStatus;
    if (!(Status & XHCI_PORT_CCS))
        return false;

    *Speed = (Status & XHCI_PORT_SPEED_MASK) >> 10;
    DbgPrint("USB: xHCI port %u connected at speed id %u\n", Port, *Speed);
    if (!(Status & XHCI_PORT_PP)) {
        *PortStatus = XHCI_PORT_PP;
        KeStallExecutionProcessor(20000);
        Status = *PortStatus;
    }

    uint32_t ResetBit = *Speed == 4 ? XHCI_PORT_WPR : XHCI_PORT_PR;
    uint32_t ChangeBit = *Speed == 4 ? XHCI_PORT_WRC : XHCI_PORT_PRC;
    *PortStatus = XHCI_PORT_PP | ResetBit;
    for (uint32_t Elapsed = 0; Elapsed < XHCI_TIMEOUT_US; Elapsed += 1000) {
        Status = *PortStatus;
        if ((Status & ChangeBit) && (Status & XHCI_PORT_PED)) {
            *PortStatus = XHCI_PORT_PP | ChangeBit;
            return true;
        }
        if (!(Status & XHCI_PORT_CCS))
            return false;
        KeStallExecutionProcessor(1000);
    }
    return false;
}

static bool XhciAllocateDevice(XHCI_DEVICE *Device)
{
    if (!HZ_SUCCESS(MmAllocateContiguousPages(XHCI_DEVICE_DMA_PAGES, 0x100000000ull, 1,
                                               &Device->DmaPfn)))
        return false;
    Device->Dma = MmPhysicalToVirtual(Device->DmaPfn << PAGE_SHIFT);
    if (!Device->Dma) {
        MmFreeContiguousPages(Device->DmaPfn, XHCI_DEVICE_DMA_PAGES);
        Device->DmaPfn = MM_INVALID_PFN;
        return false;
    }
    memset(Device->Dma, 0, XHCI_DEVICE_DMA_PAGES * PAGE_SIZE);
    Device->Ep0Ring = (volatile uint32_t *)(Device->Dma + XHCI_DEVICE_EP0_RING_PAGE * PAGE_SIZE);
    Device->Ep0Index = 0;
    Device->Ep0Cycle = 1;

    volatile XHCI_TRB *Ring = (volatile XHCI_TRB *)Device->Ep0Ring;
    XHCI_TRB Link = {0};
    uint64_t Base = XhciDeviceDmaPhysical(Device, XHCI_DEVICE_EP0_RING_PAGE);
    Link.ParameterLow = (uint32_t)Base;
    Link.ParameterHigh = (uint32_t)(Base >> 32);
    Link.Control = (XHCI_TRB_LINK << XHCI_TRB_TYPE_SHIFT) | XHCI_TRB_TOGGLE_CYCLE | XHCI_TRB_CYCLE;
    Ring[XHCI_RING_TRBS - 1] = Link;
    Device->Active = true;
    return true;
}

static bool XhciInitializeRings(XHCI_CONTROLLER *Controller)
{
    PFN_NUMBER DmaPfn;
    if (!HZ_SUCCESS(MmAllocateContiguousPages(XHCI_DMA_PAGES, 0x100000000ull, 1, &DmaPfn)))
        return false;
    Controller->DmaPfn = DmaPfn;
    Controller->Dma = MmPhysicalToVirtual(DmaPfn << PAGE_SHIFT);
    if (!Controller->Dma) {
        MmFreeContiguousPages(DmaPfn, XHCI_DMA_PAGES);
        Controller->DmaPfn = MM_INVALID_PFN;
        return false;
    }
    memset(Controller->Dma, 0, XHCI_DMA_PAGES * PAGE_SIZE);

    Controller->Dcbaa = (volatile uint64_t *)(Controller->Dma + XHCI_DMA_DCBAA_PAGE * PAGE_SIZE);
    Controller->CommandRing = (volatile uint32_t *)(Controller->Dma + XHCI_DMA_COMMAND_PAGE * PAGE_SIZE);
    Controller->EventRing = (volatile uint32_t *)(Controller->Dma + XHCI_DMA_EVENT_PAGE * PAGE_SIZE);
    Controller->CommandIndex = 0;
    Controller->EventIndex = 0;
    Controller->CommandCycle = 1;
    Controller->EventCycle = 1;

    volatile XHCI_TRB *CommandRing = (volatile XHCI_TRB *)Controller->CommandRing;
    XHCI_TRB Link = {0};
    uint64_t CommandBase = XhciDmaPhysical(Controller, XHCI_DMA_COMMAND_PAGE);
    Link.ParameterLow = (uint32_t)CommandBase;
    Link.ParameterHigh = (uint32_t)(CommandBase >> 32);
    Link.Control = (XHCI_TRB_LINK << XHCI_TRB_TYPE_SHIFT) | XHCI_TRB_TOGGLE_CYCLE | XHCI_TRB_CYCLE;
    CommandRing[XHCI_RING_TRBS - 1] = Link;

    XHCI_ERST_ENTRY *Erst = (XHCI_ERST_ENTRY *)(Controller->Dma + XHCI_DMA_ERST_PAGE * PAGE_SIZE);
    Erst->RingBase = XhciDmaPhysical(Controller, XHCI_DMA_EVENT_PAGE);
    Erst->RingSize = XHCI_RING_TRBS;

    uint32_t OperationalBase = Controller->CapabilityLength;
    Controller->ActiveSlots = HZ_MIN(Controller->MaxSlots, XHCI_MAX_ACTIVE_DEVICES);
    XhciWrite32(Controller, OperationalBase + XHCI_OP_CONFIG,
                (XhciRead32(Controller, OperationalBase + XHCI_OP_CONFIG) & ~0xFFu) |
                    Controller->ActiveSlots);
    XhciWrite64(Controller, OperationalBase + 0x30, XhciDmaPhysical(Controller, XHCI_DMA_DCBAA_PAGE));
    XhciWrite64(Controller, OperationalBase + 0x18,
                CommandBase | Controller->CommandCycle); /* CRCR.RCS */

    uint32_t Interrupter = Controller->RuntimeOffset + XHCI_RT_INTERRUPTER0;
    XhciWrite32(Controller, Interrupter + XHCI_RT_ERSTSZ, 1);
    XhciWrite64(Controller, Interrupter + XHCI_RT_ERSTBA,
                XhciDmaPhysical(Controller, XHCI_DMA_ERST_PAGE));
    XhciWrite64(Controller, Interrupter + XHCI_RT_ERDP,
                XhciDmaPhysical(Controller, XHCI_DMA_EVENT_PAGE));
    XhciWrite32(Controller, Interrupter, 0); /* Poll completions; do not enable MSI/interrupts. */
    __asm__ volatile("" : : : "memory");
    return true;
}

static bool XhciStartController(XHCI_CONTROLLER *Controller)
{
    KeInitializeMutex(&Controller->EventLock);
    uint8_t CapabilityLength = Controller->Registers[XHCI_CAPLENGTH];
    uint16_t Version = *(volatile uint16_t *)(Controller->Registers + XHCI_HCIVERSION);
    uint32_t Parameters = XhciRead32(Controller, XHCI_HCSPARAMS1);
    uint32_t DoorbellOffset = XhciRead32(Controller, XHCI_DBOFF) & ~3u;
    uint32_t RuntimeOffset = XhciRead32(Controller, XHCI_RTSOFF) & ~0x1Fu;

    if (CapabilityLength < 0x20 ||
        DoorbellOffset < CapabilityLength + 0x20 ||
        (uint64_t)DoorbellOffset + 4 > Controller->RegisterBytes ||
        RuntimeOffset < CapabilityLength + 0x20 ||
        (uint64_t)RuntimeOffset + XHCI_RT_INTERRUPTER0 + 0x20 > Controller->RegisterBytes)
        return false;

    Controller->CapabilityLength = CapabilityLength;
    Controller->DoorbellOffset = DoorbellOffset;
    Controller->RuntimeOffset = RuntimeOffset;
    Controller->PortCount = (Parameters >> 24) & 0xFF;
    Controller->MaxSlots = Parameters & 0xFF;
    if (!Controller->PortCount || Controller->PortCount > XHCI_MAX_PORTS || !Controller->MaxSlots ||
        (uint64_t)CapabilityLength + XHCI_OP_PORTS + (uint64_t)Controller->PortCount * 0x10 >
            Controller->RegisterBytes ||
        (uint64_t)DoorbellOffset + ((uint64_t)Controller->MaxSlots + 1) * 4 > Controller->RegisterBytes ||
        !XhciHandoffFirmware(Controller))
        return false;

    Controller->Operational = (volatile uint32_t *)(Controller->Registers + CapabilityLength);
    uint32_t Command = Controller->Operational[XHCI_USBCMD / 4];
    if (Command & XHCI_CMD_RUN) {
        Controller->Operational[XHCI_USBCMD / 4] = Command & ~XHCI_CMD_RUN;
        if (!XhciWait32(Controller, CapabilityLength + XHCI_USBSTS, XHCI_STS_HALTED, XHCI_STS_HALTED))
            return false;
    } else if (!XhciWait32(Controller, CapabilityLength + XHCI_USBSTS, XHCI_STS_HALTED, XHCI_STS_HALTED)) {
        return false;
    }

    Command = Controller->Operational[XHCI_USBCMD / 4];
    Controller->Operational[XHCI_USBCMD / 4] = Command | XHCI_CMD_RESET;
    if (!XhciWait32(Controller, CapabilityLength + XHCI_USBCMD, XHCI_CMD_RESET, 0) ||
        !XhciWait32(Controller, CapabilityLength + XHCI_USBSTS, XHCI_STS_NOT_READY, 0) ||
        !XhciInitializeRings(Controller))
        return false;

    Controller->Doorbells = (volatile uint32_t *)(Controller->Registers + DoorbellOffset);
    Command = Controller->Operational[XHCI_USBCMD / 4];
    Controller->Operational[XHCI_USBCMD / 4] = Command | XHCI_CMD_RUN;
    if (!XhciWait32(Controller, CapabilityLength + XHCI_USBSTS, XHCI_STS_HALTED, 0))
        return false;

    DbgPrint("xHCI: %02x:%02x.%u PCI %04x:%04x, v%u.%u, %u ports, command/event rings running\n",
             Controller->Pci->Bus, Controller->Pci->Device, Controller->Pci->Function,
             Controller->Pci->VendorId, Controller->Pci->DeviceId, Version >> 8, Version & 0xFF,
             Controller->PortCount);

    uint32_t SlotsAllocated = 0;
    uint32_t Limit = Controller->PortCount < XHCI_MAX_PORTS ? Controller->PortCount : XHCI_MAX_PORTS;
    for (uint32_t Port = 1; Port <= Limit && SlotsAllocated < Controller->ActiveSlots; Port++) {
        uint32_t Speed;
        if (!XhciResetPort(Controller, Port, &Speed))
            continue;
        uint8_t SlotId = 0;
        if (!XhciSubmitEnableSlot(Controller, &SlotId)) {
            DbgPrintEx(DPFLTR_WARNING_LEVEL, "xHCI: port %u reset, but Enable Slot command failed\n", Port);
            continue;
        }
        if (!SlotId || SlotId > Controller->ActiveSlots) {
            DbgPrintEx(DPFLTR_WARNING_LEVEL, "xHCI: Enable Slot returned invalid slot id %u\n", SlotId);
            continue;
        }
        SlotsAllocated++;
        DbgPrint("USB: port %u slot %u enabled (speed id %u); descriptors pending\n", Port, SlotId, Speed);
        XHCI_DEVICE *Device = &Controller->Devices[SlotId - 1];
        memset(Device, 0, sizeof(*Device));
        Device->DmaPfn = MM_INVALID_PFN;
        Device->SlotId = SlotId;
        Device->Port = (uint8_t)Port;
        Device->Speed = (uint8_t)Speed;
        if (!XhciAllocateDevice(Device)) {
            DbgPrintEx(DPFLTR_WARNING_LEVEL, "USB: port %u slot %u DMA allocation failed\n", Port, SlotId);
            continue;
        }
        if (!XhciEnumerateDevice(Controller, Device)) {
            DbgPrintEx(DPFLTR_WARNING_LEVEL, "USB: port %u slot %u descriptor enumeration failed\n", Port, SlotId);
            continue;
        }
        if (!XhciInitializeHid(Controller, Device))
            DbgPrintEx(DPFLTR_WARNING_LEVEL, "USB HID: port %u slot %u class setup failed\n", Port, SlotId);
        if (!Device->IsKeyboard && !Device->IsMouse && !XhciInitializeMassStorage(Controller, Device))
            DbgPrintEx(DPFLTR_WARNING_LEVEL, "USB MSC: port %u slot %u BOT initialization failed or unsupported\n",
                       Port, SlotId);
    }
    bool HasInputDevice = false;
    for (uint32_t i = 0; i < XHCI_MAX_ACTIVE_DEVICES; i++)
        HasInputDevice |= Controller->Devices[i].IsKeyboard || Controller->Devices[i].IsMouse;
    if (HasInputDevice) {
        ETHREAD *ThreadHandle = NULL;
        STATUS Status = PsCreateSystemThread(XhciInputThread, Controller, KI_THREAD_PRIORITY_DEFAULT,
                                             "XhciInput", &ThreadHandle);
        if (!HZ_SUCCESS(Status)) {
            DbgPrintEx(DPFLTR_WARNING_LEVEL, "USB HID: input polling thread failed (0x%08x)\n", (uint32_t)Status);
            return false;
        }
    }
    return true;
}

static STATUS XhciDriverEntry(DRIVER_OBJECT *Driver)
{
    (void)Driver;
    uint32_t Found = 0;
    PCI_DEVICE *Pci = PciFindDevice(0x0C, 0x03, 0x30, NULL);
    if (!Pci)
        return STATUS_NO_SUCH_DEVICE;
    for (; Pci;
         Pci = PciFindDevice(0x0C, 0x03, 0x30, Pci)) {
        if (XhciControllerCount == XHCI_MAX_CONTROLLERS) {
            DbgPrintEx(DPFLTR_WARNING_LEVEL, "xHCI: controller limit reached\n");
            PciSetDriverStatus(Pci, PciDriverStartFailed, "xHCI host controller");
            continue;
        }
        if (!Pci->Bar[0] || Pci->BarIsIo[0] || Pci->BarSize[0] < 0x1000) {
            DbgPrintEx(DPFLTR_WARNING_LEVEL, "xHCI: %02x:%02x.%u has an unusable BAR0\n",
                       Pci->Bus, Pci->Device, Pci->Function);
            PciSetDriverStatus(Pci, PciDriverStartFailed, "xHCI host controller");
            continue;
        }

        XHCI_CONTROLLER *Controller = &XhciControllers[XhciControllerCount];
        memset(Controller, 0, sizeof(*Controller));
        Controller->DmaPfn = MM_INVALID_PFN;
        Controller->Pci = Pci;
        Controller->RegisterBytes = Pci->BarSize[0];
        Controller->Registers = MmMapIoSpace(Pci->Bar[0], Controller->RegisterBytes, MM_CACHE_UNCACHED);
        if (!Controller->Registers) {
            PciSetDriverStatus(Pci, PciDriverStartFailed, "xHCI host controller");
            continue;
        }
        PciEnableDevice(Pci, PCI_COMMAND_MEMORY | PCI_COMMAND_BUS_MASTER);

        if (XhciStartController(Controller)) {
            PciSetDriverStatus(Pci, PciDriverStarted, "xHCI (boot HID, USB mass storage BOT)");
            Found++;
            XhciControllerCount++;
        } else {
            DbgPrintEx(DPFLTR_WARNING_LEVEL, "xHCI: %02x:%02x.%u capability, firmware handoff, or reset failed\n",
                       Pci->Bus, Pci->Device, Pci->Function);
            PciSetDriverStatus(Pci, PciDriverStartFailed, "xHCI host controller");
            if (Controller->DmaPfn != MM_INVALID_PFN) {
                /* DMA may still be active if failure happened after rings were installed. */
                XhciControllerCount++;
            } else {
                MmUnmapIoSpace((void *)Controller->Registers, Controller->RegisterBytes);
                memset(Controller, 0, sizeof(*Controller));
            }
        }
    }
    return Found ? STATUS_SUCCESS : STATUS_DEVICE_NOT_READY;
}

IO_DECLARE_BUILTIN_DRIVER(XhciBuiltinDriver, "\\Driver\\Xhci", XhciDriverEntry, IO_ORDER_DEVICE);
