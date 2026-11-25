// Small freestanding SHA-256 for public firmware-profile integrity comparisons.
// No cryptographic authentication claims; no external library/kernel crypto ABI.
#include "Profile.hpp"

namespace pcc_probe {
namespace {
constexpr uint32_t k[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};
uint32_t ror(uint32_t x, unsigned n) { return (x >> n) | (x << (32 - n)); }
void block(const uint8_t *b, uint32_t h[8]) {
    uint32_t w[64];
    for (unsigned i = 0; i < 16; ++i)
        w[i] = (uint32_t(b[i*4]) << 24) | (uint32_t(b[i*4+1]) << 16) |
               (uint32_t(b[i*4+2]) << 8) | b[i*4+3];
    for (unsigned i = 16; i < 64; ++i) {
        const uint32_t x = w[i-15], y = w[i-2];
        w[i] = w[i-16] + (ror(x,7) ^ ror(x,18) ^ (x >> 3)) + w[i-7] +
               (ror(y,17) ^ ror(y,19) ^ (y >> 10));
    }
    uint32_t a=h[0], c=h[2], b0=h[1], d=h[3], e=h[4], f=h[5], g=h[6], h0=h[7];
    for (unsigned i = 0; i < 64; ++i) {
        const uint32_t t1 = h0 + (ror(e,6) ^ ror(e,11) ^ ror(e,25)) +
                            ((e & f) ^ (~e & g)) + k[i] + w[i];
        const uint32_t t2 = (ror(a,2) ^ ror(a,13) ^ ror(a,22)) + ((a & b0) ^ (a & c) ^ (b0 & c));
        h0=g; g=f; f=e; e=d+t1; d=c; c=b0; b0=a; a=t1+t2;
    }
    h[0]+=a; h[1]+=b0; h[2]+=c; h[3]+=d; h[4]+=e; h[5]+=f; h[6]+=g; h[7]+=h0;
}
}
void sha256(Bytes in, uint8_t digest[32]) {
    uint32_t h[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    size_t at = 0;
    while (in.size - at >= 64) { block(in.data + at, h); at += 64; }
    uint8_t last[128]{};
    const size_t remain = in.size - at;
    for (size_t i = 0; i < remain; ++i) last[i] = in.data[at+i];
    last[remain] = 0x80;
    const size_t count = remain < 56 ? 64 : 128;
    const uint64_t bits = uint64_t(in.size) * 8;
    for (size_t i = 0; i < 8; ++i) last[count-1-i] = static_cast<uint8_t>(bits >> (i*8));
    block(last, h);
    if (count == 128) block(last+64, h);
    for (size_t i = 0; i < 32; ++i) digest[i] = static_cast<uint8_t>(h[i/4] >> (24-(i%4)*8));
}
bool hashMatches(Bytes in, const char *hex) {
    if (!in.data || in.size == 0 || in.size > cppc_lab::kMaxTextBytes) return false;
    uint8_t digest[32];
    sha256(in, digest);
    constexpr char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < 32; ++i)
        if (hex[i*2] != digits[digest[i] >> 4] || hex[i*2+1] != digits[digest[i] & 15]) return false;
    return hex[64] == '\0';
}
} // namespace pcc_probe
