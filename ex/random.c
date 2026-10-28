/*
 * Nexora OS - ex/random.c
 * Bounded user-mode random-byte service backed exclusively by the x86 RDRAND instruction.
 */
#include "ex.h"
#include "mm.h"
#include "rtl.h"
#include "uapi/hzapi.h"
#include "x86.h"

#define EXP_RDRAND_RETRIES 10u

static void ExpScrubRandomBuffer(volatile uint8_t *Buffer, uint32_t Length)
{
    while (Length--)
        *Buffer++ = 0;
}

static bool ExpCpuHasRdrand(void)
{
    uint32_t Registers[4];
    __cpuid(0, 0, Registers);
    if (Registers[0] < 1)
        return false;
    __cpuid(1, 0, Registers);
    return (Registers[2] & (1u << 30)) != 0;
}

static bool ExpReadRdrand(uint64_t *Value)
{
    for (uint32_t Attempt = 0; Attempt < EXP_RDRAND_RETRIES; ++Attempt) {
        unsigned char Ready;
        __asm__ volatile("rdrand %0; setc %1" : "=r"(*Value), "=qm"(Ready) : : "cc");
        if (Ready)
            return true;
    }
    return false;
}

STATUS NtGenerateRandomBytes(const uint64_t *Args)
{
    void *UserBuffer = (void *)Args[0];
    uint64_t RequestedLength = Args[1];
    if (RequestedLength > HZ_RANDOM_MAX_BYTES)
        return STATUS_BUFFER_TOO_SMALL;
    uint32_t Length = (uint32_t)RequestedLength;
    if (!Length)
        return STATUS_SUCCESS;

    /* Probe writability without changing the caller's existing bytes. */
    uint8_t Original[HZ_RANDOM_MAX_BYTES];
    STATUS Status = MmCopyFromUser(Original, UserBuffer, Length);
    if (!HZ_SUCCESS(Status))
        return Status;
    Status = MmCopyToUser(UserBuffer, Original, Length);
    ExpScrubRandomBuffer(Original, Length);
    if (!HZ_SUCCESS(Status))
        return Status;

    if (!ExpCpuHasRdrand())
        return STATUS_NOT_SUPPORTED;

    uint8_t Random[HZ_RANDOM_MAX_BYTES];
    uint32_t Offset = 0;
    while (Offset < Length) {
        uint64_t Word = 0;
        if (!ExpReadRdrand(&Word)) {
            ExpScrubRandomBuffer((volatile uint8_t *)Random, Length);
            ExpScrubRandomBuffer((volatile uint8_t *)&Word, sizeof(Word));
            return STATUS_UNSUCCESSFUL;
        }
        uint32_t Take = HZ_MIN((uint32_t)sizeof(Word), Length - Offset);
        memcpy(Random + Offset, &Word, Take);
        Offset += Take;
        ExpScrubRandomBuffer((volatile uint8_t *)&Word, sizeof(Word));
    }

    Status = MmCopyToUser(UserBuffer, Random, Length);
    ExpScrubRandomBuffer((volatile uint8_t *)Random, Length);
    return Status;
}
