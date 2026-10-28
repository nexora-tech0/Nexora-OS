/*
 * Nexora OS - init/inbv.c
 * Boot display: the black splash (mark plus spinner) and the stop screen used by bug checks.
 */
#include "bootinfo.h"
#include "bootvid.h"
#include "hzversion.h"
#include "inbv.h"
#include "rtl.h"

#define SPLASH_BACKGROUND 0x000000
#define SPLASH_MARK 0xC9DDF2    /* pale blue Nexora mark on black */
#define SPLASH_SPINNER 0xFFFFFF
#define STOP_BG 0x2A0E17
#define STOP_ACCENT 0xFF9E5E
#define STOP_TEXT 0xFFFFFF
#define STOP_DIM 0xE3C9CF

static struct {
    bool Ready;
    uint32_t Frame;
    uint32_t Phase; /* spinner position, in 1/256 of a revolution */
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

/* sin(2*pi*i/256) in Q15; the loader uses it to place a smooth, thick arc under the mark. */
static const int16_t SplashSin[256] = {
    0,     804,   1608,  2410,  3212,  4011,  4808,  5602,  6393,  7179,  7962,  8739,  9512,
    10278, 11039, 11793, 12539, 13279, 14010, 14732, 15446, 16151, 16846, 17530, 18204, 18868,
    19519, 20159, 20787, 21403, 22005, 22594, 23170, 23731, 24279, 24811, 25329, 25832, 26319,
    26790, 27245, 27683, 28105, 28510, 28898, 29268, 29621, 29956, 30273, 30571, 30852, 31113,
    31356, 31580, 31785, 31971, 32137, 32285, 32412, 32521, 32609, 32678, 32728, 32757, 32767,
    32757, 32728, 32678, 32609, 32521, 32412, 32285, 32137, 31971, 31785, 31580, 31356, 31113,
    30852, 30571, 30273, 29956, 29621, 29268, 28898, 28510, 28105, 27683, 27245, 26790, 26319,
    25832, 25329, 24811, 24279, 23731, 23170, 22594, 22005, 21403, 20787, 20159, 19519, 18868,
    18204, 17530, 16846, 16151, 15446, 14732, 14010, 13279, 12539, 11793, 11039, 10278, 9512,
    8739,  7962,  7179,  6393,  5602,  4808,  4011,  3212,  2410,  1608,  804,   0,     -804,
    -1608, -2410, -3212, -4011, -4808, -5602, -6393, -7179, -7962, -8739, -9512, -10278, -11039,
    -11793, -12539, -13279, -14010, -14732, -15446, -16151, -16846, -17530, -18204, -18868,
    -19519, -20159, -20787, -21403, -22005, -22594, -23170, -23731, -24279, -24811, -25329,
    -25832, -26319, -26790, -27245, -27683, -28105, -28510, -28898, -29268, -29621, -29956,
    -30273, -30571, -30852, -31113, -31356, -31580, -31785, -31971, -32137, -32285, -32412,
    -32521, -32609, -32678, -32728, -32757, -32767, -32757, -32728, -32678, -32609, -32521,
    -32412, -32285, -32137, -31971, -31785, -31580, -31356, -31113, -30852, -30571, -30273,
    -29956, -29621, -29268, -28898, -28510, -28105, -27683, -27245, -26790, -26319, -25832,
    -25329, -24811, -24279, -23731, -23170, -22594, -22005, -21403, -20787, -20159, -19519,
    -18868, -18204, -17530, -16846, -16151, -15446, -14732, -14010, -13279, -12539, -11793,
    -11039, -10278, -9512, -8739, -7962, -7179, -6393, -5602, -4808, -4011, -3212, -2410,
    -1608, -804,
};

#define SPLASH_COS(Step) ((int32_t)SplashSin[(((Step) + 64) & 255)])
#define SPLASH_SIN(Step) ((int32_t)SplashSin[(Step) & 255])

#define SPLASH_FADE_FRAMES 14    /* the mark is opaque after ~230 ms at the 60 Hz cadence */
#define SPLASH_SPINNER_START 8   /* the ring appears as the mark reaches full brightness */
#define SPLASH_SPINNER_SEGMENTS 28 /* overlapping discs form one thick, continuous bar */
#define SPLASH_SPINNER_SPEED 4   /* one smooth sweep per second at the 60 Hz cadence */

static void InbvDrawSpinner(int Width, int Height, int MarkSize, int MarkY)
{
    int Radius = HZ_MAX(12, HZ_MIN(MarkSize * 15 / 100, 30));
    int StrokeRadius = HZ_MAX(4, Radius / 4);
    int Reach = Radius + StrokeRadius + 4;
    int CenterX = Width / 2;
    int CenterY = HZ_MIN(Height - Reach - 8, MarkY + MarkSize + Height * 9 / 100);

    Inbv.Phase = (Inbv.Phase + SPLASH_SPINNER_SPEED) & 255;
    /* Clear the whole ring in one write so no dot leaves a trail behind it. */
    VidFillRect(CenterX - Reach, CenterY - Reach, Reach * 2, Reach * 2, SPLASH_BACKGROUND);
    for (int I = 0; I < SPLASH_SPINNER_SEGMENTS; I++) {
        /* Closely spaced, overlapping antialiased discs make a single curved bar with a soft tail. */
        uint32_t Step = (Inbv.Phase - (uint32_t)I * 2) & 255;
        uint32_t Alpha = 255 - (uint32_t)I * 190 / SPLASH_SPINNER_SEGMENTS;
        int32_t BarX = ((int32_t)CenterX << 16) + ((int32_t)Radius * SPLASH_COS(Step)) * 2;
        int32_t BarY = ((int32_t)CenterY << 16) + ((int32_t)Radius * SPLASH_SIN(Step)) * 2;
        VidFillDiscAA(BarX, BarY, (int32_t)StrokeRadius << 16,
                      VidBlend(SPLASH_BACKGROUND, SPLASH_SPINNER, Alpha), SPLASH_BACKGROUND);
    }
}

void InbvAnimate(void)
{
    if (!Inbv.Ready)
        return;
    KIRQL OldIrql;
    KeAcquireSpinLockRaiseToLevel(&InbvLock, HIGH_LEVEL, &OldIrql);
    if (Inbv.Ready) {
        int W = VidWidth(), H = VidHeight();
        int MarkSize = HZ_MAX(64, HZ_MIN(H / 6, 160));
        int MarkY = H * 42 / 100 - MarkSize / 2;
        Inbv.Frame++;

        /* The mark fades in once. Its antialiased fill is the most expensive drawing here, so it is
           not redrawn after it has reached full opacity. */
        if (Inbv.Frame <= SPLASH_FADE_FRAMES) {
            uint32_t Remaining = SPLASH_FADE_FRAMES - Inbv.Frame;
            uint32_t Fade = 255 - Remaining * Remaining * 255 / (SPLASH_FADE_FRAMES * SPLASH_FADE_FRAMES);
            VidDrawLogoTinted((W - MarkSize) / 2, MarkY, MarkSize, SPLASH_BACKGROUND,
                              VidBlend(SPLASH_BACKGROUND, SPLASH_MARK, Fade));
        }
        if (Inbv.Frame >= SPLASH_SPINNER_START)
            InbvDrawSpinner(W, H, MarkSize, MarkY);
    }
    KeReleaseSpinLock(&InbvLock, OldIrql);
}

bool InbvInitialize(void)
{
    if (!KeLoaderBlock.HasFramebuffer || !VidInitialize(&KeLoaderBlock.Framebuffer))
        return false;

    KeInitializeSpinLock(&InbvLock);
    Inbv.Frame = 0;
    Inbv.Phase = 0;
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
