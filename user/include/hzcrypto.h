/*
 * Nexora OS - user/include/hzcrypto.h
 * Small cryptographic helpers for user mode (M11): SHA-256 (FIPS 180-4), random bytes, and the
 * salted, iterated password hash the shell, first-boot setup and Settings share.
 *
 * Password scheme: Salt = 16 random bytes; H(0) = SHA-256(Salt || Password) and
 * H(i) = SHA-256(H(i-1) || Salt || Password) for i = 1 .. kPasswordRounds - 1. shell.ini keeps
 * "PasswordSalt" and "PasswordHash" as lower-case hex. No PasswordHash (or an empty one) means
 * the account has no password. This keeps the password out of the file; it is not meant to resist
 * someone who can read the disk and has time for a dictionary attack.
 *
 * Random bytes come from RDRAND when the processor has it, mixed with the time stamp counter, the
 * clock, the uptime and the process id, and whitened with SHA-256 (there is no kernel entropy pool).
 */
#ifndef HZCRYPTO_H
#define HZCRYPTO_H

#include "hzcore.h"

namespace hz::crypto {

constexpr size_t kSha256Size = 32;
constexpr int kPasswordRounds = 10000;

class Sha256 {
public:
    Sha256();
    void Update(const void *Data, size_t Length);
    void Final(uint8_t Digest[kSha256Size]);

private:
    void Block(const uint8_t *Block);
    uint32_t State_[8];
    uint8_t Buffer_[64];
    size_t Buffered_ = 0;
    uint64_t Total_ = 0;
};

/* One-shot digest as lower-case hex (64 characters). */
String Sha256Hex(const void *Data, size_t Length);
String ToHex(const uint8_t *Bytes, size_t Length);

void RandomBytes(void *Out, size_t Length);

String NewSalt(); /* 16 random bytes as hex */
String HashPassword(const String &Password, const String &SaltHex);
/* Compares in time independent of where the hashes differ. */
bool CheckPassword(const String &Password, const String &SaltHex, const String &HashHex);

}  // namespace hz::crypto

#endif /* HZCRYPTO_H */
