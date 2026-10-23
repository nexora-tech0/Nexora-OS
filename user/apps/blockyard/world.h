/*
 * Nexora OS - user/apps/blockyard/world.h
 * Blockyard's world: a 256 x 64 x 256 grid of blocks (one byte each), generated from a seed
 * (value-noise terrain, beaches, water at sea level, snow caps and trees) and saved as a
 * run-length encoded file.
 */
#ifndef BLOCKYARD_WORLD_H
#define BLOCKYARD_WORLD_H

#include "hzcore.h"
#include "hzsys.h"

namespace by {

enum Block : uint8_t {
    Air,
    Grass,
    Dirt,
    Stone,
    Cobblestone,
    Planks,
    Log,
    Leaves,
    Sand,
    Glass,
    Brick,
    Water,
    Bedrock,
    Snow,
    BlockCount
};

constexpr int kSizeX = 256, kSizeY = 64, kSizeZ = 256;
constexpr int kSeaLevel = 24;

const char *BlockName(uint8_t B);
/* Collision: everything but air and water. */
inline bool IsSolid(uint8_t B)
{
    return B != Air && B != Water;
}

class World {
public:
    bool Create(); /* allocates the grid */
    void Generate(uint32_t Seed);

    uint8_t Get(int X, int Y, int Z) const
    {
        if (Y < 0)
            return Bedrock;
        if ((unsigned)X >= (unsigned)kSizeX || (unsigned)Y >= (unsigned)kSizeY || (unsigned)Z >= (unsigned)kSizeZ)
            return Air;
        return Data_[Index(X, Y, Z)];
    }
    /* The raw cell, for the renderer's inner loop (in range only). */
    uint8_t At(int X, int Y, int Z) const { return Data_[Index(X, Y, Z)]; }
    void Set(int X, int Y, int Z, uint8_t B);
    /* The highest solid block's Y in a column (-1 if none). */
    int Surface(int X, int Z) const;

    uint32_t Seed() const { return Seed_; }
    uint32_t Edits() const { return Edits_; }
    void ClearEdits() { Edits_ = 0; }

    /* "HZBY" file: header, then (count, block) runs. */
    STATUS Save(const hz::String &Path, uint32_t *Bytes) const;
    STATUS Load(const hz::String &Path);

private:
    static size_t Index(int X, int Y, int Z) { return ((size_t)Y * kSizeZ + (size_t)Z) * kSizeX + (size_t)X; }
    void PlantTree(int X, int Y, int Z, uint32_t Hash);

    uint8_t *Data_ = nullptr;
    uint32_t Seed_ = 0;
    uint32_t Edits_ = 0;
};

/* Deterministic hashes and value noise (shared with the textures). */
uint32_t Hash(int32_t X, int32_t Y, uint32_t Seed);
float Hash01(int32_t X, int32_t Y, uint32_t Seed);
float ValueNoise(float X, float Y, uint32_t Seed); /* smooth, 0..1 */

}  // namespace by

#endif /* BLOCKYARD_WORLD_H */
