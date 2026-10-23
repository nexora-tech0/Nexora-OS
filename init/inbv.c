/*
 * Nexora OS - init/inbv.c
 * Boot display: the black Nexora logo-and-spinner splash and the stop screen used by bug checks.
 */
#include "bootinfo.h"
#include "bootvid.h"
#include "hzversion.h"
#include "inbv.h"
#include "rtl.h"

#define SPLASH_BACKGROUND 0x000000
#define STOP_BG 0x2A0E17
#define STOP_ACCENT 0xFF9E5E
#define STOP_TEXT 0xFFFFFF
#define STOP_DIM 0xE3C9CF

static struct {
    bool Ready;
    uint32_t Frame;
} Inbv;

/*
 * Drawing uses global clip state and threads can be preempted mid-draw, so the animation runs
 * under InbvLock at HIGH_LEVEL. The stop screen deliberately skips the lock (the interrupted
 * code may hold it).
 */
static KSPIN_LOCK InbvLock;

void InbvDisplayString(const char *Text)
{
    (void)Text;
}

void InbvEnableDisplayString(bool Enable)
{
    (void)Enable;
}

void InbvHandOffDisplay(void)
{
    /* Under the lock, so no animation frame is half drawn when win32k takes over. */
    KIRQL OldIrql;
    KeAcquireSpinLockRaiseToLevel(&InbvLock, HIGH_LEVEL, &OldIrql);
    Inbv.Ready = false;
    KeReleaseSpinLock(&InbvLock, OldIrql);
}

bool InbvIsDisplayReady(void)
{
    return Inbv.Ready;
}

void InbvSetStatusText(const char *Text)
{
    (void)Text;
}

void InbvAnimate(void)
{
    static const int8_t DotX[24] = {0, 4, 7, 10, 12, 13, 14, 13, 12, 10, 7, 4,
                                    0, -4, -7, -10, -12, -13, -14, -13, -12, -10, -7, -4};
    static const int8_t DotY[24] = {-14, -13, -12, -10, -7, -4, 0, 4, 7, 10, 12, 13,
                                    14, 13, 12, 10, 7, 4, 0, -4, -7, -10, -12, -13};
    static const uint8_t Brightness[12] = {48, 67, 86, 105, 124, 143, 162, 181, 200, 219, 238, 255};
    if (!Inbv.Ready)
        return;
    KIRQL OldIrql;
    KeAcquireSpinLockRaiseToLevel(&InbvLock, HIGH_LEVEL, &OldIrql);
    if (Inbv.Ready) {
        int W = VidWidth(), H = VidHeight();
        int LogoSize = HZ_MAX(64, HZ_MIN(H / 6, 160));
        int LogoY = H * 42 / 100 - LogoSize / 2;
        Inbv.Frame++;
        uint32_t Fade = HZ_MIN(Inbv.Frame * 22, 255);
        if (Fade < 255 || Inbv.Frame == 12)
            VidDrawLogoTinted((W - LogoSize) / 2, LogoY, LogoSize, SPLASH_BACKGROUND,
                              VidBlend(SPLASH_BACKGROUND, 0xC5CDD4, Fade));
        if (Inbv.Frame >= 3) {
            int CenterX = W / 2, CenterY = H * 86 / 100;
            uint32_t Phase = (Inbv.Frame / 2) % 24;
            VidFillRect(CenterX - 20, CenterY - 20, 40, 40, SPLASH_BACKGROUND);
            for (int I = 0; I < 12; I++) {
                uint32_t At = (Phase + (uint32_t)I * 2) % 24;
                uint32_t Gray = Brightness[I];
                uint32_t Color = (Gray << 16) | (Gray << 8) | Gray;
                VidFillRect(CenterX + DotX[At] - 1, CenterY + DotY[At] - 1, 3, 3, Color);
            }
        }
    }
    KeReleaseSpinLock(&InbvLock, OldIrql);
}

bool InbvInitialize(void)
{
    if (!KeLoaderBlock.HasFramebuffer || !VidInitialize(&KeLoaderBlock.Framebuffer))
        return false;

    KeInitializeSpinLock(&InbvLock);
    Inbv.Frame = 0;
    VidFillRect(0, 0, VidWidth(), VidHeight(), SPLASH_BACKGROUND);
    Inbv.Ready = true;
    return true;
}


/* ---- Stop screen -------------------------------------------------------------------------- */

typedef struct _STOP_LAYOUT {
    int Left, Right, Y;
    bool Compact; /* short screens (< 900 px): tighter spacing, smaller logo */
} STOP_LAYOUT;

static void InbvStopText(STOP_LAYOUT *L, const BOOT_FONT *Font, const char *Text, uint32_t Color, int After)
{
    VidDrawText(Font, L->Left, L->Y + Font->Ascent, Text, Color, STOP_BG);
    L->Y += Font->LineHeight + After;
}

static void InbvStopRegisters(STOP_LAYOUT *L, const STOP_SCREEN_INFO *Info)
{
    const KTRAP_FRAME *F = Info->Frame;
    struct {
        const char *Name;
        uint64_t Value;
    } Regs[] = {
        {"RAX", F->Rax}, {"RBX", F->Rbx}, {"RCX", F->Rcx}, {"RDX", F->Rdx}, {"RSI", F->Rsi},
        {"RDI", F->Rdi}, {"RBP", F->Rbp}, {"RSP", F->Rsp}, {"R8 ", F->R8},  {"R9 ", F->R9},
        {"R10", F->R10}, {"R11", F->R11}, {"R12", F->R12}, {"R13", F->R13}, {"R14", F->R14},
        {"R15", F->R15}, {"RIP", F->Rip}, {"RFL", F->RFlags}, {"CR2", Info->Cr2}, {"CR3", Info->Cr3},
        {"CS ", F->SegCs}, {"SS ", F->SegSs},
    };
    const int Count = (int)HZ_ARRAYSIZE(Regs);
    int CharWidth = VidMeasureText(&FontMono13, "0");
    int CellWidth = CharWidth * 24; /* "RAX 0x0000000000000000" + gap */
    int Columns = HZ_MAX(1, HZ_MIN(4, (L->Right - L->Left) / CellWidth));
    int Rows = (Count + Columns - 1) / Columns;
    int LineHeight = FontMono13.LineHeight + 1;

    for (int i = 0; i < Count; i++) {
        char Cell[40];
        RtlFormatString(Cell, sizeof(Cell), "%s 0x%016llX", Regs[i].Name, (unsigned long long)Regs[i].Value);
        int Row = i / Columns, Col = i % Columns;
        VidDrawText(&FontMono13, L->Left + Col * CellWidth, L->Y + FontMono13.Ascent + Row * LineHeight, Cell,
                    STOP_DIM, STOP_BG);
    }
    L->Y += Rows * LineHeight;
}

void InbvDrawStopScreen(const STOP_SCREEN_INFO *Info)
{
    if (!VidIsReady())
        return;
    int W = VidWidth(), H = VidHeight();
    VidResetClip();
    VidFillRect(0, 0, W, H, STOP_BG);

    STOP_LAYOUT L;
    L.Compact = H < 900;
    L.Left = HZ_MAX(32, W / 10);
    L.Right = W - L.Left;
    L.Y = L.Compact ? H / 14 : H / 9;
    int Gap = L.Compact ? 12 : 20;
    VidSetClip(L.Left, 0, L.Right - L.Left, H);

    int LogoSize = L.Compact ? 40 : 56;
    VidDrawLogo(L.Left, L.Y, LogoSize, STOP_BG);
    L.Y += LogoSize + Gap;
    InbvStopText(&L, &FontDisplay44, "Nexora OS has stopped", STOP_TEXT, Gap / 2);
    VidFillRect(L.Left, L.Y, 96, 3, STOP_ACCENT);
    L.Y += 3 + Gap;

    InbvStopText(&L, &FontUi16, "A fatal error occurred, and the system was halted to protect your data.",
                 STOP_DIM, 2);
    InbvStopText(&L, &FontUi16,
                 "The details below were also written to the serial port (COM1). Restart the computer to continue.",
                 STOP_DIM, Gap);

    char Line[320];
    RtlFormatString(Line, sizeof(Line), "Stop code: %s (0x%08X)", Info->CodeName, Info->Code);
    InbvStopText(&L, &FontUiSemi24, Line, STOP_TEXT, Gap / 2);

    if (Info->Message && Info->Message[0])
        InbvStopText(&L, &FontUi16, Info->Message, STOP_TEXT, 2);
    if (Info->File && Info->File[0]) {
        RtlFormatString(Line, sizeof(Line), "Location: %s, line %u", Info->File, Info->Line);
        InbvStopText(&L, &FontUi16, Line, STOP_DIM, 2);
    }
    if (Info->ThreadName)
        RtlFormatString(Line, sizeof(Line), "Thread '%s' (id %u) at IRQL %u", Info->ThreadName, Info->ThreadId,
                        Info->Irql);
    else
        RtlFormatString(Line, sizeof(Line), "Phase 0 initialization at IRQL %u", Info->Irql);
    InbvStopText(&L, &FontUi16, Line, STOP_DIM, 2);
    L.Y += Gap / 2;

    RtlFormatString(Line, sizeof(Line), "Parameters  0x%016llX  0x%016llX  0x%016llX  0x%016llX",
                    (unsigned long long)Info->Parameters[0], (unsigned long long)Info->Parameters[1],
                    (unsigned long long)Info->Parameters[2], (unsigned long long)Info->Parameters[3]);
    if (VidMeasureText(&FontMono13, Line) <= L.Right - L.Left) {
        InbvStopText(&L, &FontMono13, Line, STOP_DIM, Gap);
    } else {
        RtlFormatString(Line, sizeof(Line), "Parameters  0x%016llX  0x%016llX",
                        (unsigned long long)Info->Parameters[0], (unsigned long long)Info->Parameters[1]);
        InbvStopText(&L, &FontMono13, Line, STOP_DIM, 1);
        RtlFormatString(Line, sizeof(Line), "            0x%016llX  0x%016llX",
                        (unsigned long long)Info->Parameters[2], (unsigned long long)Info->Parameters[3]);
        InbvStopText(&L, &FontMono13, Line, STOP_DIM, Gap);
    }

    if (Info->Frame) {
        RtlFormatString(Line, sizeof(Line), "Registers (vector %llu, error code 0x%llX)",
                        (unsigned long long)Info->Frame->Vector, (unsigned long long)Info->Frame->ErrorCode);
        InbvStopText(&L, &FontUiSemi16, Line, STOP_TEXT, 4);
        InbvStopRegisters(&L, Info);
        L.Y += Gap;
    }

    if (Info->FrameCount > 0) {
        InbvStopText(&L, &FontUiSemi16, "Stack trace", STOP_TEXT, 4);
        int Columns = (L.Right - L.Left) >= 1100 && Info->FrameCount > 6 ? 2 : 1;
        uint32_t MaxFrames = Columns == 2 ? 12 : 8;
        uint32_t Count = HZ_MIN(Info->FrameCount, MaxFrames);
        int Rows = (int)((Count + (uint32_t)Columns - 1) / (uint32_t)Columns);
        int ColumnWidth = (L.Right - L.Left) / Columns;
        int LineHeight = FontMono13.LineHeight + 1;
        for (uint32_t i = 0; i < Count; i++) {
            char Symbol[160];
            KeFormatStackFrame(Symbol, sizeof(Symbol), Info->Frames[i], !(Info->Frame && i == 0));
            RtlFormatString(Line, sizeof(Line), "#%-2u 0x%016llX  %s", i, (unsigned long long)Info->Frames[i],
                            Symbol);
            int Col = (int)i / Rows, Row = (int)i % Rows;
            VidSetClip(L.Left + Col * ColumnWidth, 0, ColumnWidth - 16, H);
            VidDrawText(&FontMono13, L.Left + Col * ColumnWidth, L.Y + FontMono13.Ascent + Row * LineHeight, Line,
                        STOP_DIM, STOP_BG);
        }
    }
    VidResetClip();
}
