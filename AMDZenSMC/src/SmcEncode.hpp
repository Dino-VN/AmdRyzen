//
// SmcEncode.hpp — integer-only encoders for SMC key values.
//
// Header-only and free of kernel dependencies so that tests/test_smc_encode.cpp can
// check every bit pattern on the host. Nothing here may use float or double: the
// encoders run in the AMDZenSMC timer thread, and their results are what
// readAccess() copies inside VirtualSMC's trap path, where the trampoline does not
// save xmm registers (docs/research/smc-keys.md §1.5).
//
// Inputs are AMDZenPM's integer units (milli-degrees C, milli-watts, milli-volts,
// milli-amps). Outputs are "cells": a uint32_t whose in-memory bytes are exactly
// the SMC data bytes, so the consumer can memcpy the first 2 (sp) or 4 (flt) bytes
// of the cell without knowing the type.
//
#ifndef AMDZEN_SMC_ENCODE_HPP
#define AMDZEN_SMC_ENCODE_HPP

#include <stdint.h>

#if !defined(__BYTE_ORDER__) || (__BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__)
#error "SmcEncode.hpp assumes a little-endian host (x86_64 kernel, x86_64/arm64 tests)"
#endif

namespace smc {

// Fraction bits of the signed fixed-point types: spXY has X integer bits including
// the sign and 15 - X fraction bits, Y is that fraction count in hex.
constexpr unsigned kSp78Frac = 8;   // °C,  ±127.996, LSB 1/256
constexpr unsigned kSp96Frac = 6;   // W,   ±511.98,  LSB 1/64
constexpr unsigned kSp3cFrac = 12;  // V,   ±7.9998,  LSB 1/4096

// Upper bound used to keep 64-bit intermediate products far from overflow. Any
// input beyond it saturates every 16-bit type anyway.
constexpr int64_t kMilliLimit = 1ll << 40;

// Signed fixed-point raw value for milli/1000 with `frac` fraction bits.
// Rounds half away from zero (symmetric for negatives) and saturates to the int16
// range. Apple stores negatives in two's complement (MacPro7,1 dump: TCXR = C180 =
// -62.5); VirtualSMC's own encodeSp() uses sign-magnitude, so do not compare with it
// below zero.
constexpr int32_t spRaw(int64_t milli, unsigned frac) {
    if (milli > kMilliLimit) milli = kMilliLimit;
    if (milli < -kMilliLimit) milli = -kMilliLimit;
    const bool neg = milli < 0;
    const uint64_t a = (uint64_t)(neg ? -milli : milli);
    uint64_t q = ((a << frac) + 500u) / 1000u;
    if (!neg && q > 32767u) q = 32767u;
    if (neg && q > 32768u) q = 32768u;
    return neg ? -(int32_t)q : (int32_t)q;
}

// Cell for a 2-byte big-endian value: byte 0 = high byte, byte 1 = low byte.
constexpr uint32_t cellBE16(uint16_t v) {
    return (uint32_t)(v >> 8) | ((uint32_t)(v & 0xFFu) << 8);
}

constexpr uint32_t cellSp(int64_t milli, unsigned frac) {
    return cellBE16((uint16_t)(spRaw(milli, frac) & 0xFFFF));
}

// IEEE-754 binary32 bits of milli/1000, correctly rounded (nearest, ties to even),
// computed with 64-bit integers only. Inputs are clamped to the int32 range.
//   |milli| = a. Scale a by 2^s with the smallest s such that a * 2^s >= 1000 * 2^23,
//   so q = a * 2^s / 1000 has exactly 24 significant bits (2^23 <= q < 2^24) and the
//   value is q * 2^-s, i.e. biased exponent 127 + 23 - s. a < 2^32 keeps s <= 33 and
//   a << s < 2^64. Rounding up to 2^24 renormalises to 2^23 with s - 1.
constexpr uint32_t fltBits(int64_t milli) {
    if (milli > INT32_MAX) milli = INT32_MAX;
    if (milli < INT32_MIN) milli = INT32_MIN;
    if (milli == 0) return 0;
    const uint32_t sign = milli < 0 ? 0x80000000u : 0u;
    const uint64_t a = (uint64_t)(milli < 0 ? -milli : milli);
    const uint64_t target = 1000ull << 23;
    int s = 0;
    while ((a << s) < target) s++;
    const uint64_t n = a << s;
    uint64_t q = n / 1000u;
    const uint64_t r = n % 1000u;
    if (r > 500u || (r == 500u && (q & 1u))) q++;
    if (q == (1ull << 24)) { q >>= 1; s--; }
    const uint32_t exp = (uint32_t)(127 + 23 - s);
    return sign | (exp << 23) | (uint32_t)(q & 0x7FFFFFu);
}

// "flt " keys hold the float in host (little-endian) byte order, as on real Macs
// (MacPro7,1 PC0R = 0A A0 0A 42 = 34.656 W) and as VirtualSMC's valueWithFlt does.
constexpr uint32_t cellFlt(int64_t milli) { return fltBits(milli); }

// ---- key-level encoders --------------------------------------------------------

// Temperatures are clamped to >= 0: Stats decodes sp78 without a sign, so a
// negative reading would show as about 200 °C.
constexpr uint32_t encTempSp78(int32_t mC) { return cellSp(mC < 0 ? 0 : mC, kSp78Frac); }
constexpr uint32_t encPowerSp96(uint32_t mW) { return cellSp(mW, kSp96Frac); }
constexpr uint32_t encVoltSp3c(uint32_t mV) { return cellSp(mV, kSp3cFrac); }
constexpr uint32_t encFlt(uint32_t milli) { return cellFlt(milli); }

} // namespace smc

#endif // AMDZEN_SMC_ENCODE_HPP
