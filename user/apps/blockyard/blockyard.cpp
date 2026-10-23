/*
 * Nexora OS - user/apps/blockyard/blockyard.cpp
 * Blockyard: an original block-building sandbox. Walk a generated landscape, break blocks and
 * place nine kinds of building blocks. The world is drawn by the software renderer (render.h)
 * into a low-resolution frame that is scaled up into the window, one worker thread per processor.
 *
 * Controls: click to capture the mouse (the window manager's mouse lock), then the mouse looks
 * around; W A S D move, Space jumps (swims, flies up), Ctrl sprints, Shift flies down, F toggles
 * flying; left button breaks, right button places, middle button picks the block looked at;
 * 1-9 or the wheel choose a block; arrow keys also look; Esc releases the mouse; F1 help, F3
 * details, R resolution, V view distance, Ctrl+S saves. The world is saved to the documents
 * folder ("Blockyard.world") on exit and every two minutes after changes, and loaded at start.
 *
 * "blockyard.exe [seed]" makes a new world from that seed instead. Notable steps are logged
 * ("blockyard: ...") for the smoke tests.
 */
#include "hzfile.h"
#include "hzmath.h"
#include "hzui.h"
#include "render.h"

using namespace hzui;

namespace {

constexpr float kPi = 3.14159265f;
constexpr float kHalfWidth = 0.3f, kHeight = 1.8f, kEye = 1.62f;
constexpr float kReach = 6.0f;
constexpr uint64_t kAutosaveUs = 120000000;
const uint8_t kHotbar[9] = {by::Grass, by::Dirt, by::Stone, by::Cobblestone, by::Planks,
                            by::Log,   by::Leaves, by::Glass, by::Brick};

inline float FloorF(float V)
{
    float T = (float)(int32_t)V;
    return T > V ? T - 1 : T;
}

inline float ClampF(float V, float Lo, float Hi)
{
    return V < Lo ? Lo : (V > Hi ? Hi : V);
}

class GameView final : public Element {
public:
    GameView(by::World *W, by::Renderer *R, hz::String SavePath) : World_(W), Renderer_(R), SavePath_(hz::Move(SavePath))
    {
        Focusable = true;
        Spawn();
        LastSaveUs_ = NowUs();
    }

    ~GameView() override
    {
        free(Frame_.Pixels);
        free(Back_);
    }

    bool Locked() const { return Locked_; }

    /* Raw keys from the window (key-up included). */
    void KeyChanged(uint32_t Key, bool Down, uint16_t Modifiers)
    {
        if (Key < 256)
            Keys_[Key] = Down;
        if (!Down)
            return;
        bool Ctrl = (Modifiers & HZ_MOD_CTRL) != 0;
        if (Key >= '1' && Key <= '9') {
            Select((int)(Key - '1'));
        } else if (Key == HZ_VK_ESCAPE) {
            if (Locked_)
                SetLock(false);
        } else if (Key == 'F' && !Ctrl) {
            Flying_ = !Flying_;
            Vel_[1] = 0;
            Toast(Flying_ ? "Flying" : "Walking");
            HzPrint("blockyard: %s", Flying_ ? "flying" : "walking");
        } else if (Key == HZ_VK_F1) {
            ShowHelp_ = !ShowHelp_;
        } else if (Key == HZ_VK_F1 + 2) {
            Details_ = !Details_;
        } else if (Key == 'R' && !Ctrl) {
            Scale_ = Scale_ >= 4 ? 2 : Scale_ + 1;
            Toast(Scale_ == 2 ? "Resolution: high" : (Scale_ == 3 ? "Resolution: medium" : "Resolution: low"));
        } else if (Key == 'V' && !Ctrl) {
            float D = Renderer_->ViewDistance;
            Renderer_->ViewDistance = D >= 96 ? 32 : (D >= 64 ? 96 : (D >= 48 ? 64 : 48));
            Toast(hz::String::Format("View distance: %d blocks", (int)Renderer_->ViewDistance).CStr());
        } else if (Key == 'S' && Ctrl) {
            Save(true);
        }
    }

    void Look(int32_t Dx, int32_t Dy)
    {
        if (!Locked_)
            return;
        Yaw_ += Dx * 0.0035f;
        Pitch_ = ClampF(Pitch_ - Dy * 0.0035f, -1.55f, 1.55f);
    }

    void SetLock(bool On)
    {
        Window *W = GetWindow();
        if (!W)
            return;
        STATUS Status = NtUserSetWindowAttribute(W->Handle(), HZ_WA_MOUSE_LOCK, On ? 1 : 0);
        if (On && !HZ_SUCCESS(Status))
            return;
        Locked_ = On;
        for (bool &K : Keys_)
            K = false;
        HzPrint("blockyard: mouse %s", On ? "captured" : "released");
        Invalidate();
    }

    void LockLost()
    {
        if (!Locked_)
            return;
        Locked_ = false;
        for (bool &K : Keys_)
            K = false;
        HzPrint("blockyard: mouse released (window deactivated)");
        Invalidate();
    }

    bool Save(bool Announce)
    {
        uint32_t Bytes = 0;
        uint64_t Start = NowUs();
        STATUS Status = World_->Save(SavePath_, &Bytes);
        LastSaveUs_ = NowUs();
        if (!HZ_SUCCESS(Status)) {
            HzPrint("blockyard: cannot save to %s (0x%08X)", SavePath_.CStr(), (uint32_t)Status);
            if (Announce)
                Toast("The world cannot be saved here");
            return false;
        }
        World_->ClearEdits();
        HzPrint("blockyard: saved the world to %s (%u bytes, %u ms)", SavePath_.CStr(), Bytes,
                (uint32_t)((NowUs() - Start) / 1000));
        if (Announce)
            Toast("World saved");
        return true;
    }

    /* ---- Element ---- */

    bool Animate() override
    {
        uint64_t Now = NowUs();
        float Dt = LastTickUs_ ? (float)(Now - LastTickUs_) / 1e6f : 0.016f;
        LastTickUs_ = Now;
        if (Dt > 0.05f)
            Dt = 0.05f;
        Simulate(Dt);
        Renderer_->CloudOffset += Dt * 0.02f;
        /* Paused (not captured): a few frames a second are enough behind the overlay. */
        if (Locked_ || ShowHelp_ || Now - LastFrameUs_ > 250000) {
            RenderFrame();
            LastFrameUs_ = Now;
            Invalidate();
        }
        if (World_->Edits() && Now - LastSaveUs_ > kAutosaveUs)
            Save(false);
        return true;
    }

    void Paint(Painter &P) override
    {
        if (!Animating_) {
            Animating_ = true;
            StartAnimation();
            RenderFrame();
        }
        /* Everything goes to a back buffer first and is then copied in one pass: the compositor
           reads the window surface while it is painted, and the HUD drawn over a fresh frame
           would otherwise flicker. */
        const Rect &B = Bounds();
        size_t Need = (size_t)B.Width * (size_t)B.Height;
        if (Need != BackSize_) {
            free(Back_);
            Back_ = Need ? (uint32_t *)malloc(Need * 4) : nullptr;
            BackSize_ = Back_ ? Need : 0;
        }
        Rect Area = B.Intersect(P.Clip());
        if (!Back_ || Area.Empty()) {
            Blit(P);
            PaintHud(P);
            return;
        }
        HZ_CANVAS Off;
        Off.Stride = B.Width;
        Off.Pixels = Back_ - ((intptr_t)B.Y * B.Width + B.X); /* window coordinates, like the surface */
        Off.Width = B.Right();
        Off.Height = B.Bottom();
        Off.Clip = Area.ToHz();
        Painter Q(Off);
        Blit(Q);
        PaintHud(Q);
        HZ_CANVAS &C = P.Canvas();
        for (int Y = Area.Y; Y < Area.Bottom(); Y++)
            memcpy(C.Pixels + (size_t)Y * C.Stride + Area.X, Off.Pixels + (intptr_t)Y * Off.Stride + Area.X,
                   (size_t)Area.Width * 4);
    }

    void OnPointerDown(PointerEvent &E) override
    {
        E.Handled = true;
        if (!Locked_) {
            SetLock(true);
            return;
        }
        if (E.Button == HZ_MOUSE_LEFT)
            Break();
        else if (E.Button == HZ_MOUSE_RIGHT)
            Place();
        else if (E.Button == HZ_MOUSE_MIDDLE)
            Pick();
    }

    void OnPointerWheel(PointerEvent &E) override
    {
        E.Handled = true;
        int Step = E.WheelDelta > 0 ? -1 : 1;
        Select((Selected_ + Step + 9) % 9);
    }

    void OnWindowActivated(bool Active) override
    {
        if (!Active)
            LockLost();
    }

private:
    /* ---- World and player ---- */

    void Spawn()
    {
        /* The nearest dry column to the middle of the world, spiralling out. */
        int Cx = by::kSizeX / 2, Cz = by::kSizeZ / 2;
        for (int R = 0; R < 100; R++) {
            for (int Dz = -R; Dz <= R; Dz++) {
                for (int Dx = -R; Dx <= R; Dx++) {
                    if (Dx != -R && Dx != R && Dz != -R && Dz != R)
                        continue;
                    int X = Cx + Dx, Z = Cz + Dz;
                    int Y = World_->Surface(X, Z);
                    if (Y >= by::kSeaLevel && World_->Get(X, Y + 1, Z) == by::Air && World_->Get(X, Y, Z) != by::Leaves) {
                        Pos_[0] = X + 0.5f;
                        Pos_[1] = (float)Y + 1.0f;
                        Pos_[2] = Z + 0.5f;
                        Vel_[0] = Vel_[1] = Vel_[2] = 0;
                        return;
                    }
                }
            }
        }
        Pos_[0] = Cx + 0.5f;
        Pos_[1] = by::kSizeY - 2.0f;
        Pos_[2] = Cz + 0.5f;
    }

    bool Collides(float X, float Y, float Z) const
    {
        int X0 = (int)FloorF(X - kHalfWidth), X1 = (int)FloorF(X + kHalfWidth - 0.0001f);
        int Y0 = (int)FloorF(Y), Y1 = (int)FloorF(Y + kHeight - 0.0001f);
        int Z0 = (int)FloorF(Z - kHalfWidth), Z1 = (int)FloorF(Z + kHalfWidth - 0.0001f);
        for (int Cy = Y0; Cy <= Y1; Cy++)
            for (int Cz = Z0; Cz <= Z1; Cz++)
                for (int Cx = X0; Cx <= X1; Cx++)
                    if (by::IsSolid(World_->Get(Cx, Cy, Cz)))
                        return true;
        return false;
    }

    /* Moves along one axis in short steps; a blocked step is shortened by halves. */
    void MoveAxis(int Axis, float Distance)
    {
        while (Distance != 0) {
            float Step = ClampF(Distance, -0.4f, 0.4f);
            float Try[3] = {Pos_[0], Pos_[1], Pos_[2]};
            float Done = Step;
            Try[Axis] = Pos_[Axis] + Done;
            int Halvings = 0;
            while (Collides(Try[0], Try[1], Try[2]) && Halvings++ < 10) {
                Done *= 0.5f;
                Try[Axis] = Pos_[Axis] + Done;
            }
            if (Collides(Try[0], Try[1], Try[2]))
                Done = 0;
            Pos_[Axis] += Done;
            if (Done != Step) {
                if (Axis == 1 && Step < 0)
                    OnGround_ = true;
                Vel_[Axis] = 0;
                return;
            }
            Distance -= Step;
        }
    }

    bool InWater() const
    {
        return World_->Get((int)FloorF(Pos_[0]), (int)FloorF(Pos_[1] + 0.4f), (int)FloorF(Pos_[2])) == by::Water;
    }

    void Simulate(float Dt)
    {
        if (Keys_[HZ_VK_LEFT])
            Yaw_ -= 2.2f * Dt;
        if (Keys_[HZ_VK_RIGHT])
            Yaw_ += 2.2f * Dt;
        if (Keys_[HZ_VK_UP])
            Pitch_ = ClampF(Pitch_ + 1.8f * Dt, -1.55f, 1.55f);
        if (Keys_[HZ_VK_DOWN])
            Pitch_ = ClampF(Pitch_ - 1.8f * Dt, -1.55f, 1.55f);
        if (Yaw_ > kPi)
            Yaw_ -= 2 * kPi;
        if (Yaw_ < -kPi)
            Yaw_ += 2 * kPi;

        float Forward = (Keys_['W'] ? 1.0f : 0.0f) - (Keys_['S'] ? 1.0f : 0.0f);
        float Strafe = (Keys_['D'] ? 1.0f : 0.0f) - (Keys_['A'] ? 1.0f : 0.0f);
        float Length = (float)HzSqrt(Forward * Forward + Strafe * Strafe);
        if (Length > 0) {
            Forward /= Length;
            Strafe /= Length;
        }
        bool Water = InWater();
        float Speed = Flying_ ? 10.9f : (Keys_[HZ_VK_CONTROL] ? 5.6f : 4.3f);
        if (Water && !Flying_)
            Speed *= 0.6f;
        float Sy = (float)HzSin(Yaw_), Cy = (float)HzCos(Yaw_);
        Vel_[0] = (Sy * Forward + Cy * Strafe) * Speed;
        Vel_[2] = (Cy * Forward - Sy * Strafe) * Speed;
        if (Flying_) {
            Vel_[1] = (Keys_[HZ_VK_SPACE] ? Speed : 0.0f) - (Keys_[HZ_VK_SHIFT] ? Speed : 0.0f);
        } else if (Water) {
            Vel_[1] = Keys_[HZ_VK_SPACE] ? 3.2f : ClampF(Vel_[1] - 9.0f * Dt, -2.5f, 8.0f);
        } else {
            if (Keys_[HZ_VK_SPACE] && OnGround_)
                Vel_[1] = 8.4f;
            Vel_[1] = ClampF(Vel_[1] - 28.0f * Dt, -40.0f, 40.0f);
        }
        OnGround_ = false;
        MoveAxis(0, Vel_[0] * Dt);
        MoveAxis(2, Vel_[2] * Dt);
        MoveAxis(1, Vel_[1] * Dt);
        if (Pos_[1] < -20) {
            HzPrint("blockyard: fell out of the world; back to the start");
            Spawn();
        }
        UpdateTarget();
    }

    by::Camera MakeCamera() const
    {
        by::Camera C;
        C.X = Pos_[0];
        C.Y = Pos_[1] + kEye;
        C.Z = Pos_[2];
        C.Yaw = Yaw_;
        C.Pitch = Pitch_;
        C.Fov = 70 * kPi / 180;
        return C;
    }

    /* The block under the crosshair within reach (DDA as in the renderer; water is skipped). */
    void UpdateTarget()
    {
        by::Camera C = MakeCamera();
        float Cp = (float)HzCos(C.Pitch);
        float Dx = Cp * (float)HzSin(C.Yaw), Dy = (float)HzSin(C.Pitch), Dz = Cp * (float)HzCos(C.Yaw);
        int X = (int)FloorF(C.X), Y = (int)FloorF(C.Y), Z = (int)FloorF(C.Z);
        int Px = X, Py = Y, Pz = Z;
        int Sx = Dx > 0 ? 1 : -1, Sy = Dy > 0 ? 1 : -1, Sz = Dz > 0 ? 1 : -1;
        float Ax = Dx != 0 ? 1 / (Dx > 0 ? Dx : -Dx) : 1e30f, Ay = Dy != 0 ? 1 / (Dy > 0 ? Dy : -Dy) : 1e30f;
        float Az = Dz != 0 ? 1 / (Dz > 0 ? Dz : -Dz) : 1e30f;
        float Mx = Dx != 0 ? (Dx > 0 ? X + 1 - C.X : C.X - X) * Ax : 1e30f;
        float My = Dy != 0 ? (Dy > 0 ? Y + 1 - C.Y : C.Y - Y) * Ay : 1e30f;
        float Mz = Dz != 0 ? (Dz > 0 ? Z + 1 - C.Z : C.Z - Z) * Az : 1e30f;
        Target_.Valid = false;
        for (int i = 0; i < 64; i++) {
            Px = X;
            Py = Y;
            Pz = Z;
            float T;
            if (Mx < My && Mx < Mz) {
                X += Sx;
                T = Mx;
                Mx += Ax;
            } else if (My < Mz) {
                Y += Sy;
                T = My;
                My += Ay;
            } else {
                Z += Sz;
                T = Mz;
                Mz += Az;
            }
            if (T > kReach)
                return;
            uint8_t B = World_->Get(X, Y, Z);
            if (B != by::Air && B != by::Water && Y >= 0 && Y < by::kSizeY) {
                Target_ = by::Target{true, X, Y, Z, Px, Py, Pz};
                return;
            }
        }
    }

    void Break()
    {
        if (!Target_.Valid)
            return;
        uint8_t B = World_->Get(Target_.X, Target_.Y, Target_.Z);
        if (B == by::Bedrock) {
            Toast("Bedrock cannot be broken");
            return;
        }
        /* Water flows into a hole next to it (one step, no simulation). */
        uint8_t Fill = by::Air;
        static const int Sides[5][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 0, 1}, {0, 0, -1}, {0, 1, 0}};
        for (const auto &S : Sides)
            if (World_->Get(Target_.X + S[0], Target_.Y + S[1], Target_.Z + S[2]) == by::Water)
                Fill = by::Water;
        World_->Set(Target_.X, Target_.Y, Target_.Z, Fill);
        HzPrint("blockyard: broke %s at %d %d %d", by::BlockName(B), Target_.X, Target_.Y, Target_.Z);
        UpdateTarget();
    }

    void Place()
    {
        if (!Target_.Valid)
            return;
        int X = Target_.Px, Y = Target_.Py, Z = Target_.Pz;
        if (Y < 0 || Y >= by::kSizeY || X < 0 || X >= by::kSizeX || Z < 0 || Z >= by::kSizeZ)
            return;
        /* Not inside the player. */
        bool Overlaps = X + 1 > Pos_[0] - kHalfWidth && X < Pos_[0] + kHalfWidth && Y + 1 > Pos_[1] &&
                        Y < Pos_[1] + kHeight && Z + 1 > Pos_[2] - kHalfWidth && Z < Pos_[2] + kHalfWidth;
        if (Overlaps)
            return;
        uint8_t B = kHotbar[Selected_];
        World_->Set(X, Y, Z, B);
        HzPrint("blockyard: placed %s at %d %d %d", by::BlockName(B), X, Y, Z);
        UpdateTarget();
    }

    void Pick()
    {
        if (!Target_.Valid)
            return;
        uint8_t B = World_->Get(Target_.X, Target_.Y, Target_.Z);
        for (int i = 0; i < 9; i++)
            if (kHotbar[i] == B)
                Select(i);
    }

    void Select(int Index)
    {
        Selected_ = Index;
        Toast(by::BlockName(kHotbar[Index]));
    }

    void Toast(const char *Text)
    {
        Toast_ = Text;
        ToastUs_ = NowUs();
        Invalidate();
    }

    /* ---- Drawing ---- */

    void RenderFrame()
    {
        const Rect &B = Bounds();
        int W = B.Width / Scale_, H = B.Height / Scale_;
        if (W < 16 || H < 16)
            return;
        if (W != Frame_.Width || H != Frame_.Height) {
            free(Frame_.Pixels);
            Frame_.Pixels = (uint32_t *)malloc((size_t)W * H * 4);
            Frame_.Width = Frame_.Pixels ? W : 0;
            Frame_.Height = Frame_.Pixels ? H : 0;
            if (!Frame_.Pixels)
                return;
        }
        uint64_t Start = NowUs();
        by::Target Shown = Locked_ ? Target_ : by::Target{};
        Renderer_->Render(*World_, MakeCamera(), Shown, Frame_);
        uint64_t Now = NowUs();
        FrameUs_ = (uint32_t)(Now - Start);
        Frames_++;
        if (Now - FpsStartUs_ >= 1000000) {
            Fps_ = (uint32_t)(Frames_ * 1000000ull / (Now - FpsStartUs_));
            Frames_ = 0;
            FpsStartUs_ = Now;
            if (++FpsReports_ % 10 == 1)
                HzPrint("blockyard: %u fps at %dx%d on %u render thread%s (%u ms per frame)", Fps_, Frame_.Width,
                        Frame_.Height, Renderer_->Threads(), Renderer_->Threads() == 1 ? "" : "s", FrameUs_ / 1000);
        }
    }

    /* Scales the frame up into the window surface (nearest neighbour), inside the clip. */
    void Blit(Painter &P)
    {
        const Rect &B = Bounds();
        HZ_CANVAS &C = P.Canvas();
        Rect Area = B.Intersect(P.Clip());
        if (Area.Empty() || !Frame_.Pixels) {
            P.FillRect(B, 0xFF000000);
            return;
        }
        static int32_t Columns[4096];
        int Count = Area.Width < 4096 ? Area.Width : 4096;
        for (int i = 0; i < Count; i++) {
            int Sx = (Area.X + i - B.X) / Scale_;
            Columns[i] = Sx < Frame_.Width ? Sx : Frame_.Width - 1;
        }
        for (int Y = Area.Y; Y < Area.Bottom(); Y++) {
            int Sy = (Y - B.Y) / Scale_;
            if (Sy >= Frame_.Height)
                Sy = Frame_.Height - 1;
            const uint32_t *Src = Frame_.Pixels + (size_t)Sy * Frame_.Width;
            uint32_t *Dst = C.Pixels + (size_t)Y * C.Stride + Area.X;
            for (int i = 0; i < Count; i++)
                Dst[i] = Src[Columns[i]] | 0xFF000000u;
        }
    }

    /* A block's side texture, Size pixels square, at (X, Y) (inside the clip). */
    void DrawBlockIcon(Painter &P, uint8_t Block, int X, int Y, int Size)
    {
        HZ_CANVAS &C = P.Canvas();
        Rect Area = Rect{X, Y, Size, Size}.Intersect(P.Clip());
        const uint32_t *Side = Renderer_->Texture(Block, by::FaceSide);
        const uint32_t *Top = Renderer_->Texture(Block, by::FaceTop);
        for (int Py = Area.Y; Py < Area.Bottom(); Py++) {
            for (int Px = Area.X; Px < Area.Right(); Px++) {
                int Tx = (Px - X) * 16 / Size, Ty = (Py - Y) * 16 / Size;
                /* The top face on the upper quarter, so grass reads as grass. */
                uint32_t T = Ty < 4 ? Top[(Ty * 4) * 16 + Tx] : Side[Ty * 16 + Tx];
                if ((T >> 24) == 0)
                    continue;
                C.Pixels[(size_t)Py * C.Stride + Px] = T | 0xFF000000u;
            }
        }
    }

    void ShadowText(Painter &P, HZ_FONT *F, int X, int Baseline, const char *Text, Color C)
    {
        P.Text(F, X + 1, Baseline + 1, Text, 0xC0000000);
        P.Text(F, X, Baseline, Text, C);
    }

    void PaintHud(Painter &P)
    {
        const Rect &B = Bounds();
        int Cx = B.X + B.Width / 2, Cy = B.Y + B.Height / 2;
        HZ_FONT *Small = GetFont(FontRole::Caption), *Body = GetFont(FontRole::Body);
        HZ_FONT *Strong = GetFont(FontRole::BodyStrong), *Title = GetFont(FontRole::Title);

        /* Crosshair */
        if (Locked_) {
            P.FillRect(Rect{Cx - 9, Cy - 1, 19, 3}, 0x90000000);
            P.FillRect(Rect{Cx - 1, Cy - 9, 3, 19}, 0x90000000);
            P.FillRect(Rect{Cx - 8, Cy, 17, 1}, 0xF0FFFFFF);
            P.FillRect(Rect{Cx, Cy - 8, 1, 17}, 0xF0FFFFFF);
        }

        /* Hotbar */
        const int Slot = 46, Gap = 4;
        int BarWidth = 9 * Slot + 8 * Gap;
        int Bx = Cx - BarWidth / 2, By = B.Bottom() - Slot - 14;
        P.FillRoundRect(Rect{Bx - 6, By - 6, BarWidth + 12, Slot + 12}, 8, 0x8C000000);
        for (int i = 0; i < 9; i++) {
            int X = Bx + i * (Slot + Gap);
            P.FillRoundRect(Rect{X, By, Slot, Slot}, 5, i == Selected_ ? 0x60FFFFFF : 0x30FFFFFF);
            DrawBlockIcon(P, kHotbar[i], X + 7, By + 7, Slot - 14);
            if (i == Selected_)
                P.StrokeRoundRect(Rect{X - 2, By - 2, Slot + 4, Slot + 4}, 6, 2, 0xFFFFFFFF);
            char Number[2] = {(char)('1' + i), 0};
            P.Text(Small, X + 4, By + 13, Number, 0xD0FFFFFF);
        }

        /* Toast above the hotbar (2 s, fading) */
        uint64_t Age = NowUs() - ToastUs_;
        if (!Toast_.Empty() && Age < 2000000) {
            uint32_t Alpha = Age < 1500000 ? 255 : (uint32_t)(255 * (2000000 - Age) / 500000);
            int Width = HzMeasureText(Strong, Toast_.CStr());
            int Tx = Cx - Width / 2, Ty = By - 18;
            P.Text(Strong, Tx + 1, Ty + 1, Toast_.CStr(), Toast_.Length(), (Alpha * 3 / 4) << 24);
            P.Text(Strong, Tx, Ty, Toast_.CStr(), Toast_.Length(), (Alpha << 24) | 0xFFFFFF);
        }

        /* Status line */
        hz::String Status = hz::String::Format("Blockyard   %u fps   %d, %d, %d%s", Fps_, (int)FloorF(Pos_[0]),
                                               (int)FloorF(Pos_[1]), (int)FloorF(Pos_[2]), Flying_ ? "   flying" : "");
        ShadowText(P, Small, B.X + 10, B.Y + 18, Status.CStr(), 0xF0FFFFFF);
        if (Details_) {
            const char *Facing = "south";
            float Yaw = Yaw_;
            if (Yaw > -kPi / 4 && Yaw <= kPi / 4)
                Facing = "south (+Z)";
            else if (Yaw > kPi / 4 && Yaw <= 3 * kPi / 4)
                Facing = "east (+X)";
            else if (Yaw <= -kPi / 4 && Yaw > -3 * kPi / 4)
                Facing = "west (-X)";
            else
                Facing = "north (-Z)";
            hz::String Lines[4] = {
                hz::String::Format("Frame %dx%d (1/%d), %u ms, %u render threads", Frame_.Width, Frame_.Height, Scale_,
                                   FrameUs_ / 1000, Renderer_->Threads()),
                hz::String::Format("View distance %d blocks, facing %s", (int)Renderer_->ViewDistance, Facing),
                hz::String::Format("Seed %u, %u unsaved changes", World_->Seed(), World_->Edits()),
                Target_.Valid ? hz::String::Format("Looking at %s (%d, %d, %d)",
                                                   by::BlockName(World_->Get(Target_.X, Target_.Y, Target_.Z)), Target_.X,
                                                   Target_.Y, Target_.Z)
                              : hz::String("Looking at nothing in reach")};
            for (int i = 0; i < 4; i++)
                ShadowText(P, Small, B.X + 10, B.Y + 38 + i * 17, Lines[i].CStr(), 0xE0FFFFFF);
        }

        /* Pause screen / help */
        if (!Locked_ || ShowHelp_) {
            if (!Locked_)
                P.FillRect(B, 0x80000000);
            int Pw = 440, Ph = 330;
            Rect Panel{Cx - Pw / 2, Cy - Ph / 2 - 30, Pw, Ph};
            P.FillRoundRect(Panel, 10, 0xD8181A20);
            P.StrokeRoundRect(Panel, 10, 1, 0x40FFFFFF);
            P.TextLine(Title, Rect{Panel.X, Panel.Y + 18, Pw, 40}, "Blockyard", 0xFFFFFFFF, Align::Center);
            P.TextLine(Body, Rect{Panel.X, Panel.Y + 58, Pw, 22},
                       Locked_ ? "F1 closes this help" : "Click to play", Theme::Accent, Align::Center);
            static const char *const Help[][2] = {
                {"Mouse", "look around"},          {"W  A  S  D", "move"},
                {"Space", "jump, swim, fly up"},   {"Ctrl / Shift", "sprint / fly down"},
                {"Left / right button", "break / place"}, {"1 - 9, wheel", "choose a block"},
                {"F", "fly on or off"},           {"Esc", "release the mouse"},
                {"F3, R, V", "details, resolution, view distance"}, {"Ctrl+S", "save (also on exit)"}};
            for (int i = 0; i < 10; i++) {
                int Y = Panel.Y + 96 + i * 22;
                P.TextLine(Strong, Rect{Panel.X + 30, Y, 150, 20}, Help[i][0], 0xFFFFFFFF);
                P.TextLine(Body, Rect{Panel.X + 190, Y, Pw - 210, 20}, Help[i][1], 0xFFC8CCD4);
            }
        }
    }

    by::World *World_;
    by::Renderer *Renderer_;
    hz::String SavePath_;
    by::Frame Frame_;
    uint32_t *Back_ = nullptr;
    size_t BackSize_ = 0;
    by::Target Target_;
    float Pos_[3] = {0, 0, 0}, Vel_[3] = {0, 0, 0};
    float Yaw_ = 0, Pitch_ = -0.15f;
    bool OnGround_ = false, Flying_ = false;
    bool Keys_[256] = {};
    bool Locked_ = false, ShowHelp_ = false, Details_ = false, Animating_ = false;
    int Selected_ = 0;
    int Scale_ = 3;
    hz::String Toast_;
    uint64_t ToastUs_ = 0, LastTickUs_ = 0, LastFrameUs_ = 0, LastSaveUs_ = 0, FpsStartUs_ = 0;
    uint32_t Frames_ = 0, Fps_ = 0, FrameUs_ = 0, FpsReports_ = 0;
};

/* The window passes raw keys (key-up too) and the locked mouse's motion to the view. */
class GameWindow final : public Window {
public:
    GameWindow()
        : Window("Blockyard", 1000, 640,
                 HZ_WS_CAPTION | HZ_WS_RESIZABLE | HZ_WS_MINIMIZEBOX | HZ_WS_MAXIMIZEBOX | HZ_WS_VISIBLE)
    {
    }
    GameView *View = nullptr;

    int64_t HandleMessage(uint32_t Message, uint64_t WParam, uint64_t LParam) override
    {
        if (View) {
            if (Message == HZ_WM_KEYDOWN || Message == HZ_WM_KEYUP) {
                View->KeyChanged((uint32_t)WParam, Message == HZ_WM_KEYDOWN, HZ_KEY_DATA_MODIFIERS(LParam));
                if (Message == HZ_WM_KEYDOWN && (WParam == HZ_VK_ESCAPE || WParam == HZ_VK_SPACE))
                    return 0; /* not for the toolkit (Space would "click" the focused view) */
            } else if (Message == HZ_WM_MOUSEDELTA) {
                View->Look(HZ_POINT_X(LParam), HZ_POINT_Y(LParam));
                return 0;
            } else if (Message == HZ_WM_MOUSEUNLOCKED) {
                View->LockLost();
                return 0;
            }
        }
        return Window::HandleMessage(Message, WParam, LParam);
    }
};

uint32_t ProcessorCount()
{
    HZ_SYSTEM_BASIC_INFORMATION I = {};
    if (!HZ_SUCCESS(NtQuerySystemInformation(HZ_SYSTEM_BASIC_INFORMATION_CLASS, &I, sizeof(I), nullptr)))
        return 1;
    return I.ProcessorCount ? I.ProcessorCount : 1;
}

}  // namespace

extern "C" int main(int argc, char **argv)
{
    by::World *World = new by::World();
    if (!World->Create()) {
        HzPrint("blockyard: not enough memory for the world");
        return 1;
    }
    hz::String SavePath = hz::fs::Join(hz::fs::DocumentsFolder(), "Blockyard.world");
    uint64_t Start = NowUs();
    bool Loaded = false;
    if (argc > 1) {
        uint32_t Seed = 0;
        for (const char *P = argv[1]; *P >= '0' && *P <= '9'; P++)
            Seed = Seed * 10 + (uint32_t)(*P - '0');
        World->Generate(Seed);
    } else if (hz::fs::Exists(SavePath) && HZ_SUCCESS(World->Load(SavePath))) {
        Loaded = true;
    } else {
        uint64_t Now = 0;
        NtQuerySystemTime(&Now);
        World->Generate((uint32_t)(Now / 10000000ull) ^ 0x5EEDu);
    }
    HzPrint("blockyard: world %s (seed %u, %u ms)", Loaded ? "loaded" : "generated", World->Seed(),
            (uint32_t)((NowUs() - Start) / 1000));

    by::Renderer *Renderer = new by::Renderer();
    uint32_t Threads = ProcessorCount();
    if (!Renderer->Start(Threads))
        HzPrint("blockyard: only %u of %u render threads started", Renderer->Threads(), Threads);

    GameWindow *Main = new GameWindow();
    Main->SetMinimumSize(480, 320);
    Main->Background = 0xFF000000;
    GameView *View = new GameView(World, Renderer, SavePath);
    Main->View = View;
    Main->SetContent(View);
    Main->OnCloseRequested = [View]() {
        View->Save(false);
        return true;
    };
    Main->OnClosed = []() { NtUserPostQuitMessage(0); };
    if (!Main->Show()) {
        HzPrint("blockyard: cannot create the window");
        return 1;
    }
    Main->SetFocus(View);
    HzPrint("blockyard: started (%u render thread%s, saves to %s)", Renderer->Threads(),
            Renderer->Threads() == 1 ? "" : "s", SavePath.CStr());
    return RunMessageLoop();
}
