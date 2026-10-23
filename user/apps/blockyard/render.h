/*
 * Nexora OS - user/apps/blockyard/render.h
 * Blockyard's software renderer: one ray per pixel of a low-resolution frame, stepped through the
 * block grid (Amanatides-Woo DDA). The first opaque face is textured with 16 x 16 textures made
 * here from hash noise (no image files), shaded per face, fogged into the sky, seen through water
 * and glass; the sky has a gradient, a sun and drifting clouds. Rows are shared out to one worker
 * thread per processor (the M11 SMP kernel runs them in parallel); the window's thread waits.
 */
#ifndef BLOCKYARD_RENDER_H
#define BLOCKYARD_RENDER_H

#include "world.h"

namespace by {

struct Camera {
    float X, Y, Z;    /* eye position */
    float Yaw, Pitch; /* radians; yaw 0 looks along +Z, pitch up is positive */
    float Fov;        /* vertical field of view, radians */
};

struct Frame {
    uint32_t *Pixels = nullptr; /* 0xAARRGGBB, alpha ignored */
    int Width = 0, Height = 0;
};

struct Target {
    bool Valid = false;
    int X, Y, Z;    /* the block looked at */
    int Px, Py, Pz; /* the empty cell before it (where a block would be placed) */
};

enum Face : uint8_t { FaceTop, FaceSide, FaceBottom };

class Renderer {
public:
    /* Builds the textures and starts Threads workers (1 = render on the caller's thread). */
    bool Start(uint32_t Threads);
    uint32_t Threads() const { return Threads_; }

    void Render(const World &W, const Camera &C, const Target &Selected, Frame &F);

    float ViewDistance = 64; /* blocks; the fog ends here */
    float CloudOffset = 0;   /* moves the clouds */

    /* A block's texture (for the hotbar): 16 x 16 pixels. */
    const uint32_t *Texture(uint8_t B, Face Kind) const { return Textures_[B][Kind]; }

private:
    struct Job {
        Renderer *Owner;
        uint32_t Index;
        HANDLE Start, Done;
    };
    static uint32_t Worker(void *Parameter);
    void RenderRows(uint32_t First, uint32_t Step);
    uint32_t Trace(float Dx, float Dy, float Dz) const;
    uint32_t Sky(float Dx, float Dy, float Dz) const;
    static float Occlusion(const World &W, int X, int Y, int Z, int Axis, int Normal, float Hx, float Hy, float Hz);
    void BuildTextures();

    uint32_t Textures_[BlockCount][3][256];
    uint32_t Threads_ = 1;
    Job Jobs_[8];
    /* The frame being rendered (read by the workers). */
    const World *World_ = nullptr;
    Camera Camera_;
    Target Selected_;
    Frame Frame_;
    float Forward_[3], Right_[3], Up_[3];
    float TanX_ = 0, TanY_ = 0;
    bool Underwater_ = false;
};

}  // namespace by

#endif /* BLOCKYARD_RENDER_H */
