/*
 * Nexora OS - user/apps/blockyard/world.cpp
 * World generation and storage; see world.h.
 */
#include "world.h"
#include "hzfile.h"
#include "hzmath.h"

namespace by {

namespace {

const char *const kNames[BlockCount] = {"Air",   "Grass", "Dirt",  "Stone", "Cobblestone", "Planks",  "Log",
                                        "Leaves", "Sand", "Glass", "Brick", "Water",       "Bedrock", "Snow"};

constexpr uint32_t kMagic = 0x59425A48; /* "HZBY" */
constexpr uint32_t kVersion = 1;

struct FileHeader {
    uint32_t Magic, Version, Seed;
    uint32_t SizeX, SizeY, SizeZ;
    uint32_t Runs;
    uint32_t Reserved;
};

inline float Smooth(float T)
{
    return T * T * (3 - 2 * T);
}

/* Fractal value noise: Octaves layers, each twice as fine and half as strong; 0..1. */
float Fbm(float X, float Y, uint32_t Seed, int Octaves)
{
    float Sum = 0, Amplitude = 1, Total = 0;
    for (int i = 0; i < Octaves; i++) {
        Sum += ValueNoise(X, Y, Seed + (uint32_t)i * 7919u) * Amplitude;
        Total += Amplitude;
        X *= 2.03f;
        Y *= 2.03f;
        Amplitude *= 0.5f;
    }
    return Sum / Total;
}

}  // namespace

const char *BlockName(uint8_t B)
{
    return B < BlockCount ? kNames[B] : "?";
}

uint32_t Hash(int32_t X, int32_t Y, uint32_t Seed)
{
    uint32_t H = (uint32_t)X * 0x8DA6B343u ^ (uint32_t)Y * 0xD8163841u ^ Seed * 0xCB1AB31Fu;
    H ^= H >> 13;
    H *= 0x5BD1E995u;
    H ^= H >> 15;
    return H;
}

float Hash01(int32_t X, int32_t Y, uint32_t Seed)
{
    return (float)(Hash(X, Y, Seed) & 0xFFFFFF) / 16777216.0f;
}

float ValueNoise(float X, float Y, uint32_t Seed)
{
    float Fx = (float)HzFloor(X), Fy = (float)HzFloor(Y);
    int32_t Ix = (int32_t)Fx, Iy = (int32_t)Fy;
    float Tx = Smooth(X - Fx), Ty = Smooth(Y - Fy);
    float A = Hash01(Ix, Iy, Seed), B = Hash01(Ix + 1, Iy, Seed);
    float C = Hash01(Ix, Iy + 1, Seed), D = Hash01(Ix + 1, Iy + 1, Seed);
    float Top = A + (B - A) * Tx, Bottom = C + (D - C) * Tx;
    return Top + (Bottom - Top) * Ty;
}

bool World::Create()
{
    Data_ = (uint8_t *)calloc((size_t)kSizeX * kSizeY * kSizeZ, 1);
    return Data_ != nullptr;
}

void World::Set(int X, int Y, int Z, uint8_t B)
{
    if ((unsigned)X >= (unsigned)kSizeX || (unsigned)Y >= (unsigned)kSizeY || (unsigned)Z >= (unsigned)kSizeZ)
        return;
    uint8_t &Cell = Data_[Index(X, Y, Z)];
    if (Cell != B) {
        Cell = B;
        Edits_++;
    }
}

int World::Surface(int X, int Z) const
{
    for (int Y = kSizeY - 1; Y >= 0; Y--)
        if (IsSolid(Get(X, Y, Z)))
            return Y;
    return -1;
}

void World::PlantTree(int X, int Y, int Z, uint32_t H)
{
    int Trunk = 4 + (int)(H % 3);
    int Top = Y + Trunk;
    if (Top + 2 >= kSizeY)
        return;
    for (int Dy = -2; Dy <= 1; Dy++) {
        int Radius = Dy >= 0 ? 1 : 2;
        for (int Dz = -Radius; Dz <= Radius; Dz++) {
            for (int Dx = -Radius; Dx <= Radius; Dx++) {
                bool Corner = (Dx == -Radius || Dx == Radius) && (Dz == -Radius || Dz == Radius);
                if (Corner && (Dy == 1 || Hash(X + Dx * 31, Z + Dz * 17 + Dy, H) % 3 == 0))
                    continue;
                int Cx = X + Dx, Cy = Top + Dy, Cz = Z + Dz;
                if (Get(Cx, Cy, Cz) == Air)
                    Data_[Index(Cx, Cy, Cz)] = Leaves;
            }
        }
    }
    for (int i = 0; i < Trunk; i++)
        Data_[Index(X, Y + i, Z)] = Log;
}

void World::Generate(uint32_t Seed)
{
    Seed_ = Seed;
    memset(Data_, Air, (size_t)kSizeX * kSizeY * kSizeZ);
    for (int Z = 0; Z < kSizeZ; Z++) {
        for (int X = 0; X < kSizeX; X++) {
            /* Rolling hills, with mountains where a slower "continent" noise is high. */
            float Hills = Fbm(X / 48.0f, Z / 48.0f, Seed, 4);
            float Continent = Fbm(X / 160.0f, Z / 160.0f, Seed ^ 0xA5A5u, 2);
            float Mountains = Continent > 0.55f ? (Continent - 0.55f) * 70.0f : 0.0f;
            int Height = (int)(14 + Hills * 22 + Mountains);
            if (Height > kSizeY - 8)
                Height = kSizeY - 8;
            bool Beach = Height <= kSeaLevel + 1;
            bool Snowy = Height >= 47;
            for (int Y = 0; Y <= Height; Y++) {
                uint8_t B;
                if (Y == 0)
                    B = Bedrock;
                else if (Y < Height - 3)
                    B = Stone;
                else if (Y < Height)
                    B = Beach ? Sand : Dirt;
                else
                    B = Beach ? Sand : (Snowy ? Snow : Grass);
                Data_[Index(X, Y, Z)] = B;
            }
            for (int Y = Height + 1; Y <= kSeaLevel; Y++)
                Data_[Index(X, Y, Z)] = Water;
        }
    }
    /* Trees on grass, away from the edges and from each other (a hash per column). */
    for (int Z = 3; Z < kSizeZ - 3; Z++) {
        for (int X = 3; X < kSizeX - 3; X++) {
            uint32_t H = Hash(X, Z, Seed ^ 0x7EEu);
            if (H % 1000 >= 9)
                continue;
            int Y = Surface(X, Z);
            if (Y > kSeaLevel + 1 && Get(X, Y, Z) == Grass && Get(X, Y + 1, Z) == Air) {
                Data_[Index(X, Y, Z)] = Dirt;
                PlantTree(X, Y + 1, Z, H);
            }
        }
    }
    Edits_ = 0;
}

STATUS World::Save(const hz::String &Path, uint32_t *Bytes) const
{
    size_t Total = (size_t)kSizeX * kSizeY * kSizeZ;
    /* Runs of up to 65535 equal blocks: 3 bytes each. */
    hz::Vector<uint8_t> Out;
    FileHeader H = {kMagic, kVersion, Seed_, kSizeX, kSizeY, kSizeZ, 0, 0};
    Out.Resize(sizeof(H));
    for (size_t i = 0; i < Total;) {
        uint8_t B = Data_[i];
        size_t Run = 1;
        while (i + Run < Total && Data_[i + Run] == B && Run < 65535)
            Run++;
        Out.Push((uint8_t)Run);
        Out.Push((uint8_t)(Run >> 8));
        Out.Push(B);
        H.Runs++;
        i += Run;
    }
    memcpy(Out.Data(), &H, sizeof(H));
    if (Bytes)
        *Bytes = (uint32_t)Out.Size();
    return hz::fs::WriteAll(Path, (const char *)Out.Data(), Out.Size());
}

STATUS World::Load(const hz::String &Path)
{
    hz::String File;
    STATUS Status = hz::fs::ReadAll(Path, File);
    if (!HZ_SUCCESS(Status))
        return Status;
    FileHeader H;
    if (File.Length() < sizeof(H))
        return STATUS_INVALID_PARAMETER;
    memcpy(&H, File.CStr(), sizeof(H));
    if (H.Magic != kMagic || H.Version != kVersion || H.SizeX != kSizeX || H.SizeY != kSizeY || H.SizeZ != kSizeZ ||
        File.Length() < sizeof(H) + (size_t)H.Runs * 3)
        return STATUS_INVALID_PARAMETER;
    size_t Total = (size_t)kSizeX * kSizeY * kSizeZ, At = 0;
    const uint8_t *P = (const uint8_t *)File.CStr() + sizeof(H);
    for (uint32_t r = 0; r < H.Runs; r++, P += 3) {
        size_t Run = (size_t)P[0] | ((size_t)P[1] << 8);
        if (P[2] >= BlockCount || Run == 0 || At + Run > Total)
            return STATUS_INVALID_PARAMETER;
        memset(Data_ + At, P[2], Run);
        At += Run;
    }
    if (At != Total)
        return STATUS_INVALID_PARAMETER;
    Seed_ = H.Seed;
    Edits_ = 0;
    return STATUS_SUCCESS;
}

}  // namespace by
