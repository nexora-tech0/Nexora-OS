/*
 * Nexora OS - init/bootinfo.c
 * Limine boot protocol requests and their translation into KeLoaderBlock. This is the only
 * file that includes limine.h. Everything the kernel keeps is copied out of the responses,
 * because they live in bootloader-reclaimable memory that the memory manager will reuse.
 */
#include "bootinfo.h"
#include "rtl.h"

#include <limine.h>

#define HZ_LIMINE_BASE_REVISION 6

/* ---- Requests (scanned by Limine between the start and end markers) ------------------- */

HZ_USED HZ_SECTION(".limine_requests_start")
static volatile uint64_t LimineRequestsStart[] = LIMINE_REQUESTS_START_MARKER;

HZ_USED HZ_SECTION(".limine_requests")
static volatile uint64_t LimineBaseRevision[] = LIMINE_BASE_REVISION(HZ_LIMINE_BASE_REVISION);

HZ_USED HZ_SECTION(".limine_requests")
static volatile struct limine_bootloader_info_request LimineBootloaderInfo = {
    .id = LIMINE_BOOTLOADER_INFO_REQUEST_ID, .revision = 0};

HZ_USED HZ_SECTION(".limine_requests")
static volatile struct limine_firmware_type_request LimineFirmwareType = {
    .id = LIMINE_FIRMWARE_TYPE_REQUEST_ID, .revision = 0};

HZ_USED HZ_SECTION(".limine_requests")
static volatile struct limine_executable_cmdline_request LimineCmdline = {
    .id = LIMINE_EXECUTABLE_CMDLINE_REQUEST_ID, .revision = 0};

HZ_USED HZ_SECTION(".limine_requests")
static volatile struct limine_hhdm_request LimineHhdm = {.id = LIMINE_HHDM_REQUEST_ID, .revision = 0};

HZ_USED HZ_SECTION(".limine_requests")
static volatile struct limine_framebuffer_request LimineFramebuffer = {
    .id = LIMINE_FRAMEBUFFER_REQUEST_ID, .revision = 0};

HZ_USED HZ_SECTION(".limine_requests")
static volatile struct limine_memmap_request LimineMemmap = {.id = LIMINE_MEMMAP_REQUEST_ID, .revision = 0};

HZ_USED HZ_SECTION(".limine_requests")
static volatile struct limine_module_request LimineModules = {.id = LIMINE_MODULE_REQUEST_ID, .revision = 0};

HZ_USED HZ_SECTION(".limine_requests")
static volatile struct limine_rsdp_request LimineRsdp = {.id = LIMINE_RSDP_REQUEST_ID, .revision = 0};

HZ_USED HZ_SECTION(".limine_requests")
static volatile struct limine_executable_address_request LimineExecutableAddress = {
    .id = LIMINE_EXECUTABLE_ADDRESS_REQUEST_ID, .revision = 0};

HZ_USED HZ_SECTION(".limine_requests")
static volatile struct limine_date_at_boot_request LimineDateAtBoot = {
    .id = LIMINE_DATE_AT_BOOT_REQUEST_ID, .revision = 0};

/* Multiprocessor (M11): xAPIC mode (no x2APIC request; the HAL follows whatever the firmware set). */
HZ_USED HZ_SECTION(".limine_requests")
static volatile struct limine_mp_request LimineMp = {.id = LIMINE_MP_REQUEST_ID, .revision = 0, .flags = 0};

HZ_USED HZ_SECTION(".limine_requests_end")
static volatile uint64_t LimineRequestsEnd[] = LIMINE_REQUESTS_END_MARKER;

/* ---- Captured copy --------------------------------------------------------------------- */

LOADER_BLOCK KeLoaderBlock;

static char BiLoaderName[32];
static char BiLoaderVersion[32];
static char BiCommandLine[256];
static char BiModulePath[LOADER_MAX_MODULES][128];
static char BiModuleString[LOADER_MAX_MODULES][64];

static MEMORY_TYPE BiTranslateMemoryType(uint64_t LimineType)
{
    switch (LimineType) {
    case LIMINE_MEMMAP_USABLE: return MemoryUsable;
    case LIMINE_MEMMAP_ACPI_RECLAIMABLE: return MemoryAcpiReclaimable;
    case LIMINE_MEMMAP_ACPI_NVS: return MemoryAcpiNvs;
    case LIMINE_MEMMAP_BAD_MEMORY: return MemoryBad;
    case LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE: return MemoryLoaderReclaimable;
    case LIMINE_MEMMAP_EXECUTABLE_AND_MODULES: return MemoryKernelAndModules;
    case LIMINE_MEMMAP_FRAMEBUFFER: return MemoryFramebuffer;
    case LIMINE_MEMMAP_RESERVED_MAPPED: return MemoryReservedMapped;
    case LIMINE_MEMMAP_RESERVED:
    default: return MemoryReserved;
    }
}

const char *BiMemoryTypeName(MEMORY_TYPE Type)
{
    switch (Type) {
    case MemoryUsable: return "usable";
    case MemoryReserved: return "reserved";
    case MemoryAcpiReclaimable: return "ACPI reclaimable";
    case MemoryAcpiNvs: return "ACPI NVS";
    case MemoryBad: return "bad";
    case MemoryLoaderReclaimable: return "loader reclaimable";
    case MemoryKernelAndModules: return "kernel+modules";
    case MemoryFramebuffer: return "framebuffer";
    case MemoryReservedMapped: return "reserved (mapped)";
    default: return "?";
    }
}

bool BiCaptureLoaderBlock(void)
{
    LOADER_BLOCK *Lb = &KeLoaderBlock;

    if (!LIMINE_BASE_REVISION_SUPPORTED(LimineBaseRevision))
        return false;
    Lb->BaseRevision = HZ_LIMINE_BASE_REVISION;

    if (LimineBootloaderInfo.response) {
        RtlStringCopy(BiLoaderName, sizeof(BiLoaderName), LimineBootloaderInfo.response->name);
        RtlStringCopy(BiLoaderVersion, sizeof(BiLoaderVersion), LimineBootloaderInfo.response->version);
    }
    Lb->LoaderName = BiLoaderName;
    Lb->LoaderVersion = BiLoaderVersion;

    Lb->FirmwareIsUefi = LimineFirmwareType.response &&
                         LimineFirmwareType.response->firmware_type == LIMINE_FIRMWARE_TYPE_EFI64;

    if (LimineCmdline.response && LimineCmdline.response->cmdline)
        RtlStringCopy(BiCommandLine, sizeof(BiCommandLine), LimineCmdline.response->cmdline);
    Lb->CommandLine = BiCommandLine;

    Lb->HhdmOffset = LimineHhdm.response ? LimineHhdm.response->offset : 0;

    if (LimineExecutableAddress.response) {
        Lb->KernelPhysicalBase = LimineExecutableAddress.response->physical_base;
        Lb->KernelVirtualBase = LimineExecutableAddress.response->virtual_base;
    }

    Lb->Rsdp = LimineRsdp.response ? LimineRsdp.response->address : NULL;

    if (LimineDateAtBoot.response) {
        Lb->BootUnixTime = LimineDateAtBoot.response->timestamp;
        Lb->HasBootTime = true;
    }

    if (LimineFramebuffer.response && LimineFramebuffer.response->framebuffer_count > 0) {
        struct limine_framebuffer *Fb = LimineFramebuffer.response->framebuffers[0];
        if (Fb->memory_model == LIMINE_FRAMEBUFFER_RGB && Fb->bpp == 32) {
            FRAMEBUFFER_INFO *Out = &Lb->Framebuffer;
            Out->Address = Fb->address;
            Out->Width = (uint32_t)Fb->width;
            Out->Height = (uint32_t)Fb->height;
            Out->Pitch = (uint32_t)Fb->pitch;
            Out->Bpp = Fb->bpp;
            Out->RedShift = Fb->red_mask_shift;
            Out->RedSize = Fb->red_mask_size;
            Out->GreenShift = Fb->green_mask_shift;
            Out->GreenSize = Fb->green_mask_size;
            Out->BlueShift = Fb->blue_mask_shift;
            Out->BlueSize = Fb->blue_mask_size;
            Lb->HasFramebuffer = true;
        }
    }

    if (LimineMemmap.response) {
        uint64_t Count = LimineMemmap.response->entry_count;
        if (Count > LOADER_MAX_MEMORY_DESCRIPTORS)
            Count = LOADER_MAX_MEMORY_DESCRIPTORS;
        for (uint64_t i = 0; i < Count; i++) {
            struct limine_memmap_entry *E = LimineMemmap.response->entries[i];
            MEMORY_DESCRIPTOR *D = &Lb->MemoryDescriptors[i];
            D->Base = E->base;
            D->Length = E->length;
            D->Type = BiTranslateMemoryType(E->type);
            if (D->Type == MemoryUsable)
                Lb->UsableMemoryBytes += E->length;
            /* Highest RAM address (sizes the PFN database in M2); MMIO ranges don't count. */
            bool IsRam = D->Type != MemoryReserved && D->Type != MemoryReservedMapped &&
                         D->Type != MemoryFramebuffer;
            if (IsRam && E->base + E->length > Lb->HighestPhysicalAddress)
                Lb->HighestPhysicalAddress = E->base + E->length;
        }
        Lb->MemoryDescriptorCount = (uint32_t)Count;
    }

    if (LimineModules.response) {
        uint64_t Count = LimineModules.response->module_count;
        if (Count > LOADER_MAX_MODULES)
            Count = LOADER_MAX_MODULES;
        for (uint64_t i = 0; i < Count; i++) {
            struct limine_file *F = LimineModules.response->modules[i];
            BOOT_MODULE *M = &Lb->Modules[i];
            RtlStringCopy(BiModulePath[i], sizeof(BiModulePath[i]), F->path ? F->path : "");
            RtlStringCopy(BiModuleString[i], sizeof(BiModuleString[i]), F->string ? F->string : "");
            M->Path = BiModulePath[i];
            M->String = BiModuleString[i];
            M->Address = F->address;
            M->Size = F->size;
        }
        Lb->ModuleCount = (uint32_t)Count;
    }

    if (LimineMp.response) {
        uint64_t Count = LimineMp.response->cpu_count;
        if (Count > LOADER_MAX_PROCESSORS)
            Count = LOADER_MAX_PROCESSORS;
        Lb->BspLapicId = LimineMp.response->bsp_lapic_id;
        for (uint64_t i = 0; i < Count; i++) {
            struct limine_mp_info *Info = LimineMp.response->cpus[i];
            Lb->Processors[i].LapicId = Info->lapic_id;
            Lb->Processors[i].AcpiId = Info->processor_id;
            Lb->Processors[i].Mailbox = Info;
        }
        Lb->ProcessorCount = (uint32_t)Count;
    }

    return true;
}

void BiStartProcessor(const BOOT_PROCESSOR *Processor, void (*Entry)(void *Mailbox), uint64_t Argument)
{
    struct limine_mp_info *Info = Processor->Mailbox;
    Info->extra_argument = Argument;
    __atomic_store_n(&Info->goto_address, (limine_goto_address)(void *)Entry, __ATOMIC_SEQ_CST);
}

const BOOT_MODULE *BiFindModule(const char *Name)
{
    size_t NameLength = strlen(Name);
    for (uint32_t i = 0; i < KeLoaderBlock.ModuleCount; i++) {
        const BOOT_MODULE *M = &KeLoaderBlock.Modules[i];
        if (strcmp(M->String, Name) == 0)
            return M;
        size_t PathLength = strlen(M->Path);
        if (PathLength >= NameLength && strcmp(M->Path + PathLength - NameLength, Name) == 0)
            return M;
    }
    return NULL;
}

bool BiGetOptionValue(const char *Name, char *Value, size_t Size)
{
    const char *p = KeLoaderBlock.CommandLine;
    size_t Length = strlen(Name);
    if (!p || Length == 0 || Size == 0)
        return false;
    while (*p) {
        while (*p == ' ' || *p == '\t')
            p++;
        const char *Start = p;
        while (*p && *p != ' ' && *p != '\t')
            p++;
        if ((size_t)(p - Start) > Length && strncmp(Start, Name, Length) == 0 && Start[Length] == '=') {
            size_t Copy = HZ_MIN((size_t)(p - Start) - Length - 1, Size - 1);
            memcpy(Value, Start + Length + 1, Copy);
            Value[Copy] = '\0';
            return true;
        }
    }
    return false;
}

bool BiCommandLineHasOption(const char *Option)
{
    const char *p = KeLoaderBlock.CommandLine;
    size_t Length = strlen(Option);
    if (!p || Length == 0)
        return false;
    while (*p) {
        while (*p == ' ' || *p == '\t')
            p++;
        const char *Start = p;
        while (*p && *p != ' ' && *p != '\t')
            p++;
        if ((size_t)(p - Start) == Length && strncmp(Start, Option, Length) == 0)
            return true;
    }
    return false;
}
