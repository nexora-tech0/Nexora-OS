/*
 * Nexora OS - user/apps/blockyard/render.cpp
 * The software renderer; see render.h.
 */
#include "render.h"
#include "hzmath.h"

namespace by {

namespace {

constexpr uint32_t kSkyHorizon = 0xBFDCF5, kSkyZenith = 0x4F8FE0;
constexpr uint32_t kWater = 0x2D63C8;
constexpr float kSun[3] = {0.4206f, 0.5508f, 0.7210f}; /* unit length */

inline float Clamp01(float V)
{
    return V < 0 ? 0 : (V > 1 ? 1 : V);
}

inline uint32_t Rgb(int R, int G, int B)
{
    R = R < 0 ? 0 : (R > 255 ? 255 : R);
    G = G < 0 ? 0 : (G > 255 ? 255 : G);
    B = B < 0 ? 0 : (B > 255 ? 255 : B);
    return 0xFF000000u | ((uint32_t)R << 16) | ((uint32_t)G << 8) | (uint32_t)B;
}

inline uint32_t Scale(uint32_t C, float F)
{
    return Rgb((int)(((C >> 16) & 0xFF) * F), (int)(((C >> 8) & 0xFF) * F), (int)((C & 0xFF) * F)) | (C & 0xFF000000u);
}

inline uint32_t Mix(uint32_t A, uint32_t B, float T)
{
    int Ar = (A >> 16) & 0xFF, Ag = (A >> 8) & 0xFF, Ab = A & 0xFF;
    int Br = (B >> 16) & 0xFF, Bg = (B >> 8) & 0xFF, Bb = B & 0xFF;
    return Rgb(Ar + (int)((Br - Ar) * T), Ag + (int)((Bg - Ag) * T), Ab + (int)((Bb - Ab) * T));
}

/* A base color with per-pixel brightness noise (Amount is the +- range). */
inline uint32_t Speckle(uint32_t Base, int X, int Y, uint32_t Seed, float Amount)
{
    return Scale(Base, 1.0f - Amount + 2 * Amount * Hash01(X, Y, Seed));
}

inline float Floor(float V)
{
    float T = (float)(int32_t)V;
    return T > V ? T - 1 : T;
}

}  // namespace

/* ---- Textures ----------------------------------------------------------------------------------- */

void Renderer::BuildTextures()
{
    memset(Textures_, 0, sizeof(Textures_));
    for (int Y = 0; Y < 16; Y++) {
        for (int X = 0; X < 16; X++) {
            int I = Y * 16 + X;
            /* Grass: green top; dirt sides under a ragged green fringe. */
            uint32_t DirtC = Speckle(0x86603E, X, Y, 11, 0.16f);
            if (Hash01(X, Y, 12) < 0.12f)
                DirtC = Scale(DirtC, 0.75f);
            uint32_t GrassC = Speckle(0x5FA83C, X, Y, 13, 0.18f);
            Textures_[Grass][FaceTop][I] = GrassC;
            int Fringe = 3 + (int)(Hash01(X, 0, 14) * 2.5f);
            Textures_[Grass][FaceSide][I] = Y < Fringe ? Speckle(0x5A9E38, X, Y, 15, 0.15f) : DirtC;
            Textures_[Grass][FaceBottom][I] = DirtC;
            Textures_[Dirt][FaceTop][I] = Textures_[Dirt][FaceSide][I] = Textures_[Dirt][FaceBottom][I] = DirtC;

            /* Stone: grey with soft streaks. */
            float Streak = ValueNoise(X / 3.0f, Y / 6.0f, 21);
            uint32_t StoneC = Speckle(Scale(0x8C8C8C, 0.85f + Streak * 0.25f), X, Y, 22, 0.08f);
            Textures_[Stone][0][I] = Textures_[Stone][1][I] = Textures_[Stone][2][I] = StoneC;

            /* Cobblestone: rounded stones with dark joints (cells of a jittered grid). */
            int Cx = (X + (Y / 5 % 2) * 2) / 4, Cy = Y / 5;
            int Lx = (X + (Y / 5 % 2) * 2) % 4, Ly = Y % 5;
            bool Joint = Lx == 0 || Ly == 0;
            uint32_t Stone2 = Scale(0x7D7D7D, 0.8f + 0.4f * Hash01(Cx, Cy, 31));
            uint32_t CobbleC = Joint ? Speckle(0x4C4C4C, X, Y, 32, 0.1f) : Speckle(Stone2, X, Y, 33, 0.08f);
            Textures_[Cobblestone][0][I] = Textures_[Cobblestone][1][I] = Textures_[Cobblestone][2][I] = CobbleC;

            /* Planks: boards of 4 rows with a dark seam and grain, staggered ends. */
            int Board = Y / 4;
            bool Seam = Y % 4 == 3 || (X == (Board % 2 ? 4 : 11));
            uint32_t PlankBase = Scale(0xB28A55, 0.9f + 0.15f * Hash01(Board, 0, 41));
            float Grain = ValueNoise(X / 4.0f, Y * 1.5f, 42);
            uint32_t PlanksC = Seam ? Scale(PlankBase, 0.62f) : Scale(PlankBase, 0.9f + Grain * 0.18f);
            Textures_[Planks][0][I] = Textures_[Planks][1][I] = Textures_[Planks][2][I] = PlanksC;

            /* Log: bark with vertical ridges; rings on the ends. */
            float Ridge = ValueNoise(X * 1.3f, Y / 5.0f, 51);
            Textures_[Log][FaceSide][I] = Speckle(Scale(0x6B4A2B, 0.75f + Ridge * 0.4f), X, Y, 52, 0.06f);
            float Dx = X - 7.5f, Dy = Y - 7.5f;
            float Radius = (float)HzSqrt(Dx * Dx + Dy * Dy);
            bool Bark = Radius > 6.6f;
            bool Ring = ((int)(Radius * 1.1f)) % 2 == 0;
            uint32_t End = Bark ? Scale(0x6B4A2B, 0.85f) : (Ring ? 0xB8915A : 0xA07A48);
            Textures_[Log][FaceTop][I] = Textures_[Log][FaceBottom][I] = Speckle(End, X, Y, 53, 0.05f);

            /* Leaves: dense, uneven green with dark gaps. */
            float L = Hash01(X, Y, 61);
            /* Alpha 0 on the darkest pixels: gaps the rays look through. */
            uint32_t LeavesC = L < 0.14f ? 0x00000000 : Speckle(L < 0.6f ? 0x3E7D2A : 0x4C9333, X, Y, 62, 0.1f);
            Textures_[Leaves][0][I] = Textures_[Leaves][1][I] = Textures_[Leaves][2][I] = LeavesC;

            /* Sand and snow. */
            uint32_t SandC = Speckle(0xDCCB8C, X, Y, 71, 0.07f);
            Textures_[Sand][0][I] = Textures_[Sand][1][I] = Textures_[Sand][2][I] = SandC;
            uint32_t SnowC = Speckle(0xF2F6FA, X, Y, 72, 0.04f);
            Textures_[Snow][FaceTop][I] = SnowC;
            Textures_[Snow][FaceSide][I] = Y < 4 + (int)(Hash01(X, 1, 73) * 2) ? SnowC : StoneC;
            Textures_[Snow][FaceBottom][I] = StoneC;

            /* Glass: a pale frame with two glints; the rest is clear (alpha 0). */
            bool Frame = X == 0 || Y == 0 || X == 15 || Y == 15;
            bool Glint = (X - Y == 3 && X > 4 && X < 10) || (X - Y == 6 && X > 8 && X < 12);
            uint32_t GlassC = Frame ? 0xFFDDEEF4 : (Glint ? 0xFFF4FBFF : 0x00000000);
            Textures_[Glass][0][I] = Textures_[Glass][1][I] = Textures_[Glass][2][I] = GlassC;

            /* Brick: 8 x 4 bricks in running bond with light mortar. */
            int Row = Y / 4;
            int Bx = (X + (Row % 2) * 4) % 8;
            bool Mortar = Y % 4 == 3 || Bx == 7;
            uint32_t BrickBase = Scale(0x9C4A36, 0.85f + 0.25f * Hash01((X + (Row % 2) * 4) / 8, Row, 81));
            uint32_t BrickC = Mortar ? Speckle(0xC8C0B0, X, Y, 82, 0.05f) : Speckle(BrickBase, X, Y, 83, 0.07f);
            Textures_[Brick][0][I] = Textures_[Brick][1][I] = Textures_[Brick][2][I] = BrickC;

            /* Water (used for the hotbar only; the renderer tints instead) and bedrock. */
            uint32_t WaterC = Speckle(kWater, X, Y, 91, 0.1f);
            Textures_[Water][0][I] = Textures_[Water][1][I] = Textures_[Water][2][I] = WaterC;
            uint32_t BedC = Speckle(Hash01(X / 2, Y / 2, 92) < 0.5f ? 0x3A3A3A : 0x5A5A5A, X, Y, 93, 0.15f);
            Textures_[Bedrock][0][I] = Textures_[Bedrock][1][I] = Textures_[Bedrock][2][I] = BedC;
        }
    }
}

/* ---- Workers ------------------------------------------------------------------------------------ */

bool Renderer::Start(uint32_t Threads)
{
    BuildTextures();
    if (Threads < 1)
        Threads = 1;
    if (Threads > 8)
        Threads = 8;
    Threads_ = 1;
    if (Threads == 1)
        return true;
    for (uint32_t i = 0; i < Threads; i++) {
        Job &J = Jobs_[i];
        J.Owner = this;
        J.Index = i;
        if (!HZ_SUCCESS(NtCreateEvent(&J.Start, HZ_SYNCHRONIZATION_EVENT, 0)) ||
            !HZ_SUCCESS(NtCreateEvent(&J.Done, HZ_SYNCHRONIZATION_EVENT, 0)))
            return false;
        HANDLE Thread = 0;
        if (!HZ_SUCCESS(HzCreateThread(Worker, &J, &Thread, nullptr)))
            return Threads_ > 1;
        NtClose(Thread);
        Threads_ = i + 1;
    }
    return true;
}

uint32_t Renderer::Worker(void *Parameter)
{
    Job *J = static_cast<Job *>(Parameter);
    for (;;) {
        if (!HZ_SUCCESS(NtWaitForSingleObject(J->Start, 0, nullptr)))
            continue;
        J->Owner->RenderRows(J->Index, J->Owner->Threads_);
        NtSetEvent(J->Done, nullptr);
    }
    return 0;
}

void Renderer::Render(const World &W, const Camera &C, const Target &Selected, Frame &F)
{
    World_ = &W;
    Camera_ = C;
    Selected_ = Selected;
    Frame_ = F;
    float Cp = (float)HzCos(C.Pitch), Sp = (float)HzSin(C.Pitch);
    float Cy = (float)HzCos(C.Yaw), Sy = (float)HzSin(C.Yaw);
    Forward_[0] = Cp * Sy;
    Forward_[1] = Sp;
    Forward_[2] = Cp * Cy;
    Right_[0] = Cy;
    Right_[1] = 0;
    Right_[2] = -Sy;
    Up_[0] = -Sp * Sy;
    Up_[1] = Cp;
    Up_[2] = -Sp * Cy;
    TanY_ = (float)(HzSin(C.Fov / 2) / HzCos(C.Fov / 2));
    TanX_ = TanY_ * (float)F.Width / (float)(F.Height ? F.Height : 1);
    Underwater_ = W.Get((int)Floor(C.X), (int)Floor(C.Y), (int)Floor(C.Z)) == Water;

    if (Threads_ == 1) {
        RenderRows(0, 1);
        return;
    }
    for (uint32_t i = 0; i < Threads_; i++)
        NtSetEvent(Jobs_[i].Start, nullptr);
    HANDLE Done[8];
    for (uint32_t i = 0; i < Threads_; i++)
        Done[i] = Jobs_[i].Done;
    NtWaitForMultipleObjects(Threads_, Done, HZ_WAIT_ALL, nullptr);
}

/* Interleaved rows (First, First + Step, ...) balance sky and ground between the workers. */
void Renderer::RenderRows(uint32_t First, uint32_t Step)
{
    const Frame &F = Frame_;
    float InvW = 2.0f / (float)F.Width, InvH = 2.0f / (float)F.Height;
    for (int Y = (int)First; Y < F.Height; Y += (int)Step) {
        float V = (1.0f - ((float)Y + 0.5f) * InvH) * TanY_;
        uint32_t *Row = F.Pixels + (size_t)Y * F.Width;
        for (int X = 0; X < F.Width; X++) {
            float U = (((float)X + 0.5f) * InvW - 1.0f) * TanX_;
            float Dx = Forward_[0] + Right_[0] * U + Up_[0] * V;
            float Dy = Forward_[1] + Right_[1] * U + Up_[1] * V;
            float Dz = Forward_[2] + Right_[2] * U + Up_[2] * V;
            float Length = (float)HzSqrt(Dx * Dx + Dy * Dy + Dz * Dz);
            Row[X] = Trace(Dx / Length, Dy / Length, Dz / Length);
        }
    }
}

/* ---- Tracing ------------------------------------------------------------------------------------ */

uint32_t Renderer::Sky(float Dx, float Dy, float Dz) const
{
    uint32_t C = Mix(kSkyHorizon, kSkyZenith, Clamp01(Dy * 1.6f + 0.05f));
    float Dot = Dx * kSun[0] + Dy * kSun[1] + Dz * kSun[2];
    if (Dot > 0.9975f)
        return 0xFFFFF4C8;
    if (Dot > 0.985f)
        C = Mix(C, 0xFFF4D0, (Dot - 0.985f) / 0.0125f * 0.6f);
    /* Clouds: a flat layer far above, value noise cut at a threshold, fading with distance. */
    if (Dy > 0.03f) {
        float K = (96.0f - Camera_.Y) / Dy;
        if (K < 900) {
            float Px = (Camera_.X + Dx * K) / 40.0f + CloudOffset, Pz = (Camera_.Z + Dz * K) / 40.0f;
            float N = ValueNoise(Px, Pz, 101) * 0.7f + ValueNoise(Px * 2.7f, Pz * 2.7f, 102) * 0.3f;
            if (N > 0.56f)
                C = Mix(C, 0xFFFFFF, Clamp01((N - 0.56f) * 6.0f) * 0.85f * (1.0f - K / 900.0f));
        }
    }
    return C;
}

/* Ambient occlusion of a face: the cell in front of it (along its normal) and that cell's
   neighbours along the face's two axes darken the corners they touch (the usual voxel rule:
   two sides close a corner completely); the hit point's position blends the four corners. */
float Renderer::Occlusion(const World &W, int X, int Y, int Z, int Axis, int Normal, float Hx, float Hy, float Hz)
{
    int Front[3] = {X, Y, Z};
    Front[Axis] += Normal;
    static const int Tangents[3][2] = {{2, 1}, {0, 2}, {0, 1}};
    int A = Tangents[Axis][0], B = Tangents[Axis][1];
    float Hit[3] = {Hx, Hy, Hz};
    float Fa = Hit[A] - Floor(Hit[A]), Fb = Hit[B] - Floor(Hit[B]);
    auto Solid = [&](int Da, int Db) {
        int C[3] = {Front[0], Front[1], Front[2]};
        C[A] += Da;
        C[B] += Db;
        return IsSolid(W.Get(C[0], C[1], C[2])) ? 1 : 0;
    };
    int SideA0 = Solid(-1, 0), SideA1 = Solid(1, 0), SideB0 = Solid(0, -1), SideB1 = Solid(0, 1);
    if (!(SideA0 | SideA1 | SideB0 | SideB1)) {
        /* No sides: only the diagonal corners can darken. */
        if (!(Solid(-1, -1) | Solid(1, -1) | Solid(-1, 1) | Solid(1, 1)))
            return 1.0f;
    }
    auto Corner = [&](int Sa, int Sb, int SideA, int SideB) {
        int Count = (SideA && SideB) ? 3 : SideA + SideB + Solid(Sa, Sb);
        return 1.0f - 0.17f * (float)Count;
    };
    float C00 = Corner(-1, -1, SideA0, SideB0), C10 = Corner(1, -1, SideA1, SideB0);
    float C01 = Corner(-1, 1, SideA0, SideB1), C11 = Corner(1, 1, SideA1, SideB1);
    float Low = C00 + (C10 - C00) * Fa, High = C01 + (C11 - C01) * Fa;
    return Low + (High - Low) * Fb;
}

uint32_t Renderer::Trace(float Dx, float Dy, float Dz) const
{
    const World &W = *World_;
    const Camera &C = Camera_;
    float Ox = C.X, Oy = C.Y, Oz = C.Z;
    int X = (int)Floor(Ox), Y = (int)Floor(Oy), Z = (int)Floor(Oz);
    int StepX = Dx > 0 ? 1 : -1, StepY = Dy > 0 ? 1 : -1, StepZ = Dz > 0 ? 1 : -1;
    const float Big = 1e30f;
    float DeltaX = Dx != 0 ? (Dx > 0 ? 1 / Dx : -1 / Dx) : Big;
    float DeltaY = Dy != 0 ? (Dy > 0 ? 1 / Dy : -1 / Dy) : Big;
    float DeltaZ = Dz != 0 ? (Dz > 0 ? 1 / Dz : -1 / Dz) : Big;
    float MaxX = Dx != 0 ? (Dx > 0 ? (X + 1 - Ox) : (Ox - X)) * DeltaX : Big;
    float MaxY = Dy != 0 ? (Dy > 0 ? (Y + 1 - Oy) : (Oy - Y)) * DeltaY : Big;
    float MaxZ = Dz != 0 ? (Dz > 0 ? (Z + 1 - Oz) : (Oz - Z)) * DeltaZ : Big;
    float Far = ViewDistance;
    float WaterAt = Underwater_ ? 0.0f : -1.0f; /* where the ray entered water */
    bool ThroughGlass = false;
    uint32_t Result = 0;
    bool Hit = false;
    float T = 0;
    int Axis = 0;

    for (;;) {
        if (MaxX < MaxY && MaxX < MaxZ) {
            X += StepX;
            T = MaxX;
            MaxX += DeltaX;
            Axis = 0;
        } else if (MaxY < MaxZ) {
            Y += StepY;
            T = MaxY;
            MaxY += DeltaY;
            Axis = 1;
        } else {
            Z += StepZ;
            T = MaxZ;
            MaxZ += DeltaZ;
            Axis = 2;
        }
        if (T > Far)
            break;
        if (Y < 0) {
            Result = 0xFF2A2A2A;
            Hit = true;
            break;
        }
        if (Y >= kSizeY) {
            if (StepY > 0)
                break; /* above the world: only sky from here */
            continue;
        }
        if ((unsigned)X >= (unsigned)kSizeX || (unsigned)Z >= (unsigned)kSizeZ) {
            /* Outside the world: the ray can only come back if it is heading inwards. */
            if ((X < 0 && StepX < 0) || (X >= kSizeX && StepX > 0) || (Z < 0 && StepZ < 0) || (Z >= kSizeZ && StepZ > 0))
                break;
            continue;
        }
        uint8_t B = W.At(X, Y, Z);
        if (B == Air) {
            if (WaterAt >= 0 && !Underwater_)
                break; /* left the water upwards: show the surface over the sky (rare) */
            continue;
        }
        if (B == Water) {
            if (WaterAt < 0)
                WaterAt = T;
            continue;
        }
        /* Texture coordinates on the face that was entered. */
        float Hx = Ox + Dx * T, Hy = Oy + Dy * T, Hz = Oz + Dz * T;
        float U, V;
        Face Kind = FaceSide;
        if (Axis == 0) {
            U = Hz - Floor(Hz);
            V = 1 - (Hy - Floor(Hy));
        } else if (Axis == 1) {
            U = Hx - Floor(Hx);
            V = Hz - Floor(Hz);
            Kind = StepY < 0 ? FaceTop : FaceBottom;
        } else {
            U = Hx - Floor(Hx);
            V = 1 - (Hy - Floor(Hy));
        }
        int Tu = (int)(U * 16), Tv = (int)(V * 16);
        Tu = Tu < 0 ? 0 : (Tu > 15 ? 15 : Tu);
        Tv = Tv < 0 ? 0 : (Tv > 15 ? 15 : Tv);
        uint32_t Texel = Textures_[B][Kind][Tv * 16 + Tu];
        if ((Texel >> 24) == 0) { /* clear glass */
            ThroughGlass = ThroughGlass || B == Glass;
            continue;
        }
        static const float Shades[3][2] = {{0.80f, 0.80f}, {0.50f, 1.00f}, {0.66f, 0.66f}};
        float Shade = Shades[Axis][Axis == 1 ? (StepY < 0) : 0];
        /* A little depth on side faces: darker towards the bottom of each block. */
        if (Axis != 1)
            Shade *= 0.92f + 0.08f * (1 - V);
        Shade *= Occlusion(W, X, Y, Z, Axis, Axis == 0 ? -StepX : (Axis == 1 ? -StepY : -StepZ), Hx, Hy, Hz);
        Result = Scale(Texel, Shade);
        if (Selected_.Valid && X == Selected_.X && Y == Selected_.Y && Z == Selected_.Z &&
            (U < 0.055f || U > 0.945f || V < 0.055f || V > 0.945f))
            Result = Mix(Result, 0x101010, 0.75f);
        Hit = true;
        break;
    }

    uint32_t Color;
    float Distance = Hit ? T : Far;
    if (Hit) {
        float FogStart = Far * 0.55f;
        float Fog = Distance > FogStart ? Clamp01((Distance - FogStart) / (Far - FogStart)) : 0.0f;
        Color = Fog > 0 ? Mix(Result, Sky(Dx, Dy < 0.02f ? 0.02f : Dy, Dz), Fog) : Result;
    } else {
        Color = Sky(Dx, Dy, Dz);
    }
    if (ThroughGlass)
        Color = Mix(Color, 0xDDEEF8, 0.12f);
    if (WaterAt >= 0) {
        float Depth = Distance - WaterAt;
        float Amount = Underwater_ ? Clamp01(0.35f + Distance * 0.05f) : Clamp01(0.35f + Depth * 0.09f);
        Color = Mix(Color, Underwater_ ? 0x1D3F7A : kWater, Amount * 0.9f);
    }
    return Color;
}

}  // namespace by
