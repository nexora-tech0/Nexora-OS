/*
 * Nexora OS - user/lib/hzrt/hzcrypto.cpp
 * SHA-256, random bytes and password hashing; see hzcrypto.h.
 */
#include "hzcrypto.h"
#include "hzsys.h"

namespace hz::crypto {

namespace {

constexpr uint32_t kRoundConstants[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

inline uint32_t Rotr(uint32_t X, int N)
{
    return (X >> N) | (X << (32 - N));
}

inline uint64_t ReadTsc()
{
    uint32_t Low, High;
    __asm__ volatile("rdtsc" : "=a"(Low), "=d"(High));
    return ((uint64_t)High << 32) | Low;
}

bool HasRdrand()
{
    uint32_t A = 1, B, C = 0, D;
    __asm__ volatile("cpuid" : "+a"(A), "=b"(B), "+c"(C), "=d"(D));
    return (C >> 30) & 1;
}

bool Rdrand(uint64_t &Value)
{
    for (int Try = 0; Try < 10; Try++) {
        unsigned char Ok;
        __asm__ volatile("rdrand %0; setc %1" : "=r"(Value), "=qm"(Ok) : : "cc");
        if (Ok)
            return true;
    }
    return false;
}

uint64_t g_Counter;

}  // namespace

Sha256::Sha256()
{
    static const uint32_t Initial[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    memcpy(State_, Initial, sizeof(State_));
}

void Sha256::Block(const uint8_t *P)
{
    uint32_t W[64];
    for (int i = 0; i < 16; i++)
        W[i] = (uint32_t)P[i * 4] << 24 | (uint32_t)P[i * 4 + 1] << 16 | (uint32_t)P[i * 4 + 2] << 8 | P[i * 4 + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t S0 = Rotr(W[i - 15], 7) ^ Rotr(W[i - 15], 18) ^ (W[i - 15] >> 3);
        uint32_t S1 = Rotr(W[i - 2], 17) ^ Rotr(W[i - 2], 19) ^ (W[i - 2] >> 10);
        W[i] = W[i - 16] + S0 + W[i - 7] + S1;
    }
    uint32_t A = State_[0], B = State_[1], C = State_[2], D = State_[3];
    uint32_t E = State_[4], F = State_[5], G = State_[6], H = State_[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = Rotr(E, 6) ^ Rotr(E, 11) ^ Rotr(E, 25);
        uint32_t Choose = (E & F) ^ (~E & G);
        uint32_t T1 = H + S1 + Choose + kRoundConstants[i] + W[i];
        uint32_t S0 = Rotr(A, 2) ^ Rotr(A, 13) ^ Rotr(A, 22);
        uint32_t Majority = (A & B) ^ (A & C) ^ (B & C);
        uint32_t T2 = S0 + Majority;
        H = G;
        G = F;
        F = E;
        E = D + T1;
        D = C;
        C = B;
        B = A;
        A = T1 + T2;
    }
    State_[0] += A;
    State_[1] += B;
    State_[2] += C;
    State_[3] += D;
    State_[4] += E;
    State_[5] += F;
    State_[6] += G;
    State_[7] += H;
}

void Sha256::Update(const void *Data, size_t Length)
{
    const uint8_t *P = static_cast<const uint8_t *>(Data);
    Total_ += Length;
    while (Length) {
        size_t Take = Min<size_t>(64 - Buffered_, Length);
        memcpy(Buffer_ + Buffered_, P, Take);
        Buffered_ += Take;
        P += Take;
        Length -= Take;
        if (Buffered_ == 64) {
            Block(Buffer_);
            Buffered_ = 0;
        }
    }
}

void Sha256::Final(uint8_t Digest[kSha256Size])
{
    uint64_t Bits = Total_ * 8;
    uint8_t Pad = 0x80;
    Update(&Pad, 1);
    Pad = 0;
    while (Buffered_ != 56)
        Update(&Pad, 1);
    uint8_t Length[8];
    for (int i = 0; i < 8; i++)
        Length[i] = (uint8_t)(Bits >> (56 - 8 * i));
    Update(Length, 8);
    for (int i = 0; i < 8; i++) {
        Digest[i * 4] = (uint8_t)(State_[i] >> 24);
        Digest[i * 4 + 1] = (uint8_t)(State_[i] >> 16);
        Digest[i * 4 + 2] = (uint8_t)(State_[i] >> 8);
        Digest[i * 4 + 3] = (uint8_t)State_[i];
    }
}

String ToHex(const uint8_t *Bytes, size_t Length)
{
    static const char Digits[] = "0123456789abcdef";
    String Out;
    for (size_t i = 0; i < Length; i++) {
        Out.Append(Digits[Bytes[i] >> 4]);
        Out.Append(Digits[Bytes[i] & 15]);
    }
    return Out;
}

String Sha256Hex(const void *Data, size_t Length)
{
    Sha256 H;
    H.Update(Data, Length);
    uint8_t Digest[kSha256Size];
    H.Final(Digest);
    return ToHex(Digest, sizeof(Digest));
}

void RandomBytes(void *Out, size_t Length)
{
    static int UseRdrand = -1;
    if (UseRdrand < 0)
        UseRdrand = HasRdrand() ? 1 : 0;
    uint8_t *P = static_cast<uint8_t *>(Out);
    while (Length) {
        /* Each 32-byte piece hashes fresh measurements and a counter. */
        uint64_t Seed[8] = {};
        uint64_t Time = 0, Uptime = 0;
        NtQuerySystemTime(&Time);
        NtQueryUptime(&Uptime);
        Seed[0] = ReadTsc();
        Seed[1] = Time;
        Seed[2] = Uptime;
        Seed[3] = HzCurrentPeb()->ProcessId;
        Seed[4] = ++g_Counter;
        Seed[5] = (uint64_t)(uintptr_t)P;
        if (UseRdrand && (!Rdrand(Seed[6]) || !Rdrand(Seed[7])))
            UseRdrand = 0;
        Seed[7] ^= ReadTsc();
        uint8_t Digest[kSha256Size];
        Sha256 H;
        H.Update(Seed, sizeof(Seed));
        H.Final(Digest);
        size_t Take = Min<size_t>(Length, sizeof(Digest));
        memcpy(P, Digest, Take);
        P += Take;
        Length -= Take;
    }
}

String NewSalt()
{
    uint8_t Salt[16];
    RandomBytes(Salt, sizeof(Salt));
    return ToHex(Salt, sizeof(Salt));
}

String HashPassword(const String &Password, const String &SaltHex)
{
    uint8_t Digest[kSha256Size];
    Sha256 First;
    First.Update(SaltHex.CStr(), SaltHex.Length());
    First.Update(Password.CStr(), Password.Length());
    First.Final(Digest);
    for (int i = 1; i < kPasswordRounds; i++) {
        Sha256 H;
        H.Update(Digest, sizeof(Digest));
        H.Update(SaltHex.CStr(), SaltHex.Length());
        H.Update(Password.CStr(), Password.Length());
        H.Final(Digest);
    }
    return ToHex(Digest, sizeof(Digest));
}

bool CheckPassword(const String &Password, const String &SaltHex, const String &HashHex)
{
    String Computed = HashPassword(Password, SaltHex);
    if (Computed.Length() != HashHex.Length())
        return false;
    uint8_t Difference = 0;
    for (size_t i = 0; i < Computed.Length(); i++)
        Difference |= (uint8_t)(Computed[i] ^ HashHex[i]);
    return Difference == 0;
}

}  // namespace hz::crypto
