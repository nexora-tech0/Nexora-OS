/*
 * Nexora OS - include/bootinfo.h
 * Loader-independent view of what the bootloader handed us. Only init/bootinfo.c knows Limine;
 * every other component reads LOADER_BLOCK (the NT-style "loader parameter block").
 */
#ifndef BOOTINFO_H
#define BOOTINFO_H

#include "hzbase.h"

HZ_EXTERN_C_BEGIN

typedef enum _MEMORY_TYPE {
    MemoryUsable = 0,
    MemoryReserved,
    MemoryAcpiReclaimable,
    MemoryAcpiNvs,
    MemoryBad,
    MemoryLoaderReclaimable,
    MemoryKernelAndModules,
    MemoryFramebuffer,
    MemoryReservedMapped,
} MEMORY_TYPE;

typedef struct _MEMORY_DESCRIPTOR {
    uint64_t Base;
    uint64_t Length;
    MEMORY_TYPE Type;
} MEMORY_DESCRIPTOR;

typedef struct _FRAMEBUFFER_INFO {
    void *Address; /* virtual (HHDM) */
    uint32_t Width;
    uint32_t Height;
    uint32_t Pitch; /* bytes per scanline */
    uint16_t Bpp;
    uint8_t RedShift, RedSize;
    uint8_t GreenShift, GreenSize;
    uint8_t BlueShift, BlueSize;
} FRAMEBUFFER_INFO;

typedef struct _BOOT_MODULE {
    const char *Path;
    const char *String;
    void *Address; /* virtual (HHDM) */
    uint64_t Size;
} BOOT_MODULE;

#define LOADER_MAX_MEMORY_DESCRIPTORS 256
#define LOADER_MAX_MODULES 8
#define LOADER_MAX_PROCESSORS 64

/* A processor Limine started (M11). Mailbox is the bootloader's limine_mp_info, in reclaimable
   memory: only valid until the memory manager reclaims it (see KeParkApplicationProcessors). */
typedef struct _BOOT_PROCESSOR {
    uint32_t LapicId;
    uint32_t AcpiId;
    void *Mailbox;
} BOOT_PROCESSOR;

typedef struct _LOADER_BLOCK {
    const char *LoaderName;
    const char *LoaderVersion;
    uint64_t BaseRevision;
    bool FirmwareIsUefi;
    const char *CommandLine;
    uint64_t HhdmOffset;
    uint64_t KernelPhysicalBase;
    uint64_t KernelVirtualBase;
    void *Rsdp; /* virtual (HHDM), or NULL */
    int64_t BootUnixTime;
    bool HasBootTime;

    bool HasFramebuffer;
    FRAMEBUFFER_INFO Framebuffer;

    uint32_t MemoryDescriptorCount;
    MEMORY_DESCRIPTOR MemoryDescriptors[LOADER_MAX_MEMORY_DESCRIPTORS];
    uint64_t UsableMemoryBytes;
    uint64_t HighestPhysicalAddress; /* end of the highest RAM range (excludes MMIO/reserved) */

    uint32_t ModuleCount;
    BOOT_MODULE Modules[LOADER_MAX_MODULES];

    uint32_t ProcessorCount; /* including the boot processor; 0 if Limine did not report them */
    uint32_t BspLapicId;
    BOOT_PROCESSOR Processors[LOADER_MAX_PROCESSORS];
} LOADER_BLOCK;

extern LOADER_BLOCK KeLoaderBlock;

/*
 * Copies the bootloader responses into KeLoaderBlock. Returns false if the bootloader does not
 * speak the protocol revision we were built for.
 */
bool BiCaptureLoaderBlock(void);

/* Returns the module whose string (or path suffix) matches Name, or NULL. */
const BOOT_MODULE *BiFindModule(const char *Name);

/* Points a processor's mailbox at Entry (it jumps there with the mailbox in rdi); Argument is
   stored in the mailbox's extra argument first. Only before bootloader memory is reclaimed. */
void BiStartProcessor(const BOOT_PROCESSOR *Processor, void (*Entry)(void *Mailbox), uint64_t Argument);

/* True if the kernel command line contains the exact whitespace-separated Option. */
bool BiCommandLineHasOption(const char *Option);
/* Copies the value of "Name=value" from the command line; false if the option is absent. */
bool BiGetOptionValue(const char *Name, char *Value, size_t Size);

const char *BiMemoryTypeName(MEMORY_TYPE Type);

HZ_EXTERN_C_END

#endif /* BOOTINFO_H */
