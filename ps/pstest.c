/*
 * Nexora OS - ps/pstest.c
 * Boot-time process-manager check, run in Phase 1 once \??\C: exists.
 *
 * The process self-tests that used to be here started test programs from the ramdisk (hello.exe,
 * usertest.exe and crash.exe), waited for them and compared each exit status with the expected one -
 * the first user program at its preferred base and relocated, system calls and handles and threads,
 * and one faulting run per exception. Those programs were removed with the beta apps, so what is
 * left is the invariant they were there to protect: once Phase 1 has settled, the object manager
 * holds no leftover process, thread, event or file object, and the pool is back where it started.
 */
#include "psp.h"

static void PspBuildPeFixture(uint8_t Image[0x3000])
{
    memset(Image, 0, 0x3000);
    PE_DOS_HEADER Dos = {0};
    Dos.Magic = PE_DOS_MAGIC;
    Dos.NtHeaderOffset = 0x80;
    memcpy(Image, &Dos, sizeof(Dos));

    uint32_t Signature = PE_NT_SIGNATURE;
    memcpy(Image + Dos.NtHeaderOffset, &Signature, sizeof(Signature));
    PE_FILE_HEADER File = {0};
    File.Machine = PE_MACHINE_AMD64;
    File.NumberOfSections = 1;
    File.SizeOfOptionalHeader = sizeof(PE_OPTIONAL_HEADER64);
    File.Characteristics = PE_FILE_EXECUTABLE_IMAGE;
    memcpy(Image + Dos.NtHeaderOffset + sizeof(Signature), &File, sizeof(File));

    PE_OPTIONAL_HEADER64 Optional = {0};
    Optional.Magic = PE_OPTIONAL_MAGIC_PE32PLUS;
    Optional.AddressOfEntryPoint = 0x1000;
    Optional.ImageBase = 0x140000000ull;
    Optional.SectionAlignment = PAGE_SIZE;
    Optional.FileAlignment = 0x200;
    Optional.SizeOfImage = 0x2000;
    Optional.SizeOfHeaders = 0x200;
    Optional.Subsystem = 3;
    Optional.NumberOfRvaAndSizes = PE_DIRECTORY_COUNT;
    memcpy(Image + Dos.NtHeaderOffset + sizeof(Signature) + sizeof(File), &Optional, sizeof(Optional));

    PE_SECTION_HEADER Section = {0};
    Section.VirtualSize = 0x200;
    Section.VirtualAddress = 0x1000;
    Section.SizeOfRawData = 0x200;
    Section.PointerToRawData = 0x200;
    Section.Characteristics = PE_SCN_MEM_READ | PE_SCN_MEM_EXECUTE;
    memcpy(Image + Dos.NtHeaderOffset + sizeof(Signature) + sizeof(File) + sizeof(Optional), &Section,
           sizeof(Section));
}

static bool PspPeValidationSelfTests(void)
{
    static uint8_t Image[0x3000];
    PE_IMAGE_INFO Info;
    PspBuildPeFixture(Image);
    bool ValidImageAccepted = HZ_SUCCESS(PsValidatePeImage(Image, 0x3000, &Info)) &&
                              Info.Machine == PE_MACHINE_AMD64 && Info.SectionCount == 1;

    PspBuildPeFixture(Image);
    PE_FILE_HEADER *File = (PE_FILE_HEADER *)(Image + 0x80 + sizeof(uint32_t));
    File->Machine = 0x014C; /* x86 PE32 is not an AMD64 PE32+ image. */
    bool WrongArchitectureRejected = !HZ_SUCCESS(PsValidatePeImage(Image, 0x3000, NULL));

    PspBuildPeFixture(Image);
    PE_OPTIONAL_HEADER64 *Optional =
        (PE_OPTIONAL_HEADER64 *)(Image + 0x80 + sizeof(uint32_t) + sizeof(PE_FILE_HEADER));
    Optional->DataDirectory[PE_DIRECTORY_IMPORT].VirtualAddress = Optional->SizeOfImage - 4;
    Optional->DataDirectory[PE_DIRECTORY_IMPORT].Size = 8;
    bool OutOfRangeDirectoryRejected = !HZ_SUCCESS(PsValidatePeImage(Image, 0x3000, NULL));

    PspBuildPeFixture(Image);
    Optional = (PE_OPTIONAL_HEADER64 *)(Image + 0x80 + sizeof(uint32_t) + sizeof(PE_FILE_HEADER));
    Optional->DataDirectory[4].VirtualAddress = 0x2800; /* certificate directory uses a file offset */
    Optional->DataDirectory[4].Size = 0x200;
    bool SecurityFileOffsetAccepted = HZ_SUCCESS(PsValidatePeImage(Image, 0x3000, NULL));

    PspBuildPeFixture(Image);
    Optional = (PE_OPTIONAL_HEADER64 *)(Image + 0x80 + sizeof(uint32_t) + sizeof(PE_FILE_HEADER));
    Optional->DataDirectory[PE_DIRECTORY_EXPORT].VirtualAddress = 0x1000;
    Optional->DataDirectory[PE_DIRECTORY_EXPORT].Size = 0x100;
    PE_EXPORT_DIRECTORY Exports = {0};
    Exports.Base = 1;
    Exports.NumberOfFunctions = 2;
    Exports.NumberOfNames = 1;
    Exports.AddressOfFunctions = 0x1040;
    Exports.AddressOfNames = 0x1048;
    Exports.AddressOfNameOrdinals = 0x104C;
    memcpy(Image + 0x200, &Exports, sizeof(Exports));
    uint32_t FunctionTable[2] = {0x1100, 0x10A0};
    memcpy(Image + 0x240, FunctionTable, sizeof(FunctionTable));
    uint32_t NameRva = 0x1080;
    memcpy(Image + 0x248, &NameRva, sizeof(NameRva));
    uint16_t NameIndex = 1;
    memcpy(Image + 0x24C, &NameIndex, sizeof(NameIndex));
    memcpy(Image + 0x280, "ProbeExport", sizeof("ProbeExport"));
    memcpy(Image + 0x2A0, "KERNELBASE.ForwardedProbe", sizeof("KERNELBASE.ForwardedProbe"));
    PE_HEADERS Headers = {0};
    bool HeadersLocated = HZ_SUCCESS(PspLocatePeHeaders(Image, 0x3000, &Headers));
    uint32_t ExportRva = 0;
    char Forwarder[64];
    bool ForwardedExportResolved = HeadersLocated &&
                                  HZ_SUCCESS(PeFindFileExport(Image, 0x3000, &Headers,
                                                              "ProbeExport", 0, false,
                                                              &ExportRva, Forwarder, sizeof(Forwarder))) &&
                                  ExportRva == 0x10A0 &&
                                  strcmp(Forwarder, "KERNELBASE.ForwardedProbe") == 0;

    bool OrdinalExportResolved = HeadersLocated &&
                                 HZ_SUCCESS(PeFindFileExport(Image, 0x3000, &Headers, NULL, 1, true,
                                                             &ExportRva, Forwarder, sizeof(Forwarder))) &&
                                 ExportRva == 0x1100 && Forwarder[0] == 0;
    bool MissingExportRejected = HeadersLocated &&
                                 PeFindFileExport(Image, 0x3000, &Headers, "NoSuchExport", 0, false,
                                                  &ExportRva, Forwarder, sizeof(Forwarder)) == STATUS_NOT_FOUND;
    NameIndex = 2;
    memcpy(Image + 0x24C, &NameIndex, sizeof(NameIndex));
    bool BadExportOrdinalRejected = HeadersLocated &&
                                    PeFindFileExport(Image, 0x3000, &Headers, "ProbeExport", 0, false,
                                                     &ExportRva, Forwarder, sizeof(Forwarder)) ==
                                    STATUS_INVALID_IMAGE_FORMAT;

    uint8_t LoadedImage[0x2000] = {0};
    memcpy(LoadedImage + 0x1000, Image + 0x200, 0x200);
    /* Restore the valid name ordinal in the mapped copy; the file fixture above was corrupted. */
    NameIndex = 1;
    memcpy(LoadedImage + 0x104C, &NameIndex, sizeof(NameIndex));
    bool MappedNamedExportResolved = HeadersLocated &&
                                    HZ_SUCCESS(PeFindImageExport(LoadedImage, sizeof(LoadedImage), &Headers,
                                                                "ProbeExport", 0, false, &ExportRva,
                                                                Forwarder, sizeof(Forwarder))) &&
                                    ExportRva == 0x10A0 &&
                                    strcmp(Forwarder, "KERNELBASE.ForwardedProbe") == 0;
    bool MappedOrdinalExportResolved = HeadersLocated &&
                                       HZ_SUCCESS(PeFindImageExport(LoadedImage, sizeof(LoadedImage), &Headers,
                                                                   NULL, 1, true, &ExportRva,
                                                                   Forwarder, sizeof(Forwarder))) &&
                                       ExportRva == 0x1100 && Forwarder[0] == 0;
    bool MappedForwarderNeedsBuffer = HeadersLocated &&
                                     PeFindImageExport(LoadedImage, sizeof(LoadedImage), &Headers,
                                                       "ProbeExport", 0, false, &ExportRva, NULL, 0) ==
                                         STATUS_BUFFER_TOO_SMALL;
    NameIndex = 2;
    memcpy(LoadedImage + 0x104C, &NameIndex, sizeof(NameIndex));
    bool MappedBadOrdinalRejected = HeadersLocated &&
                                   PeFindImageExport(LoadedImage, sizeof(LoadedImage), &Headers,
                                                     "ProbeExport", 0, false, &ExportRva,
                                                     Forwarder, sizeof(Forwarder)) == STATUS_INVALID_IMAGE_FORMAT;

    uint32_t Passed = (uint32_t)ValidImageAccepted + (uint32_t)WrongArchitectureRejected +
                      (uint32_t)OutOfRangeDirectoryRejected + (uint32_t)SecurityFileOffsetAccepted +
                      (uint32_t)ForwardedExportResolved + (uint32_t)OrdinalExportResolved +
                      (uint32_t)MissingExportRejected + (uint32_t)BadExportOrdinalRejected +
                      (uint32_t)MappedNamedExportResolved + (uint32_t)MappedOrdinalExportResolved +
                      (uint32_t)MappedForwarderNeedsBuffer + (uint32_t)MappedBadOrdinalRejected;
    DbgPrintEx(Passed == 12 ? DPFLTR_INFO_LEVEL : DPFLTR_ERROR_LEVEL,
               "PE image/export self-tests: %u/12 passed\n", Passed);
    return Passed == 12;
}

bool PsRunSelfTests(void)
{
    bool PeValidationOk = PspPeValidationSelfTests();
    POOL_STATISTICS PoolBefore, PoolAfter;
    ExQueryPoolStatistics(&PoolBefore);
    uint64_t ProcessesBefore = PsProcessType->ObjectCount, ThreadsBefore = PsThreadType->ObjectCount;
    uint64_t EventsBefore = ExEventObjectType->ObjectCount, FilesBefore = IoFileObjectType->ObjectCount;

    /* Thread objects are deleted by the worker thread after the reaper frees their stacks. */
    for (int Wait = 0; Wait < 200; Wait++) {
        if (PsProcessType->ObjectCount == ProcessesBefore && PsThreadType->ObjectCount == ThreadsBefore)
            break;
        KeDelayExecutionThread(10000);
    }
    ExQueryPoolStatistics(&PoolAfter);
    bool NoLeaks = PsProcessType->ObjectCount == ProcessesBefore && PsThreadType->ObjectCount == ThreadsBefore &&
                   ExEventObjectType->ObjectCount == EventsBefore && IoFileObjectType->ObjectCount == FilesBefore &&
                   PoolAfter.BytesInUse == PoolBefore.BytesInUse;
    DbgPrintEx(NoLeaks ? DPFLTR_TRACE_LEVEL : DPFLTR_ERROR_LEVEL,
               "PsTest: objects left: %lld processes, %lld threads, %lld events, %lld files; pool %+lld bytes: %s\n",
               (long long)(PsProcessType->ObjectCount - ProcessesBefore),
               (long long)(PsThreadType->ObjectCount - ThreadsBefore),
               (long long)(ExEventObjectType->ObjectCount - EventsBefore),
               (long long)(IoFileObjectType->ObjectCount - FilesBefore),
               (long long)(PoolAfter.BytesInUse - PoolBefore.BytesInUse), NoLeaks ? "OK" : "LEAK");
    DbgPrintEx(NoLeaks ? DPFLTR_INFO_LEVEL : DPFLTR_ERROR_LEVEL, "Process self-tests: %u/1 passed\n",
               NoLeaks ? 1u : 0u);
    return NoLeaks && PeValidationOk;
}
