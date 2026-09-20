#include "util/Crypto.hpp"
#include "util/Encoding.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <random>

namespace sapo::util {

    namespace {

        using Bytes = std::vector<uint8_t>;

        std::string_view svOf(const Bytes &bytes) {
            return std::string_view(reinterpret_cast<const char *>(bytes.data()), bytes.size());
        }
        std::string strOf(const Bytes &bytes) { return std::string(svOf(bytes)); }
        Bytes bytesOf(std::string_view text) {
            Bytes out(text.size());
            std::memcpy(out.data(), text.data(), text.size());
            return out;
        }

        // -----------------------------------------------------------------
        // SHA-256 (FIPS 180-4)
        // -----------------------------------------------------------------
        const uint32_t kSha256K[64] = {
            0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
            0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
            0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
            0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
            0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
            0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
            0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
            0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
        };

        inline uint32_t rotr32(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
        inline uint32_t rotl32(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

        Bytes sha256(std::string_view data) {
            uint32_t h[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                             0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
            uint64_t bit_len = static_cast<uint64_t>(data.size()) * 8ull;
            Bytes buf(data.begin(), data.end());
            buf.push_back(0x80);
            while (buf.size() % 64 != 56) buf.push_back(0x00);
            for (int i = 7; i >= 0; --i) buf.push_back(static_cast<uint8_t>((bit_len >> (i * 8)) & 0xFF));

            for (size_t chunk = 0; chunk < buf.size(); chunk += 64) {
                uint32_t w[64];
                for (int i = 0; i < 16; ++i) {
                    w[i] = (static_cast<uint32_t>(buf[chunk + i * 4]) << 24) |
                           (static_cast<uint32_t>(buf[chunk + i * 4 + 1]) << 16) |
                           (static_cast<uint32_t>(buf[chunk + i * 4 + 2]) << 8) |
                           (static_cast<uint32_t>(buf[chunk + i * 4 + 3]));
                }
                for (int i = 16; i < 64; ++i) {
                    const uint32_t s0 = rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
                    const uint32_t s1 = rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
                    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
                }
                uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
                for (int i = 0; i < 64; ++i) {
                    const uint32_t S1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
                    const uint32_t ch = (e & f) ^ (~e & g);
                    const uint32_t temp1 = hh + S1 + ch + kSha256K[i] + w[i];
                    const uint32_t S0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
                    const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
                    const uint32_t temp2 = S0 + maj;
                    hh = g; g = f; f = e; e = d + temp1; d = c; c = b; b = a; a = temp1 + temp2;
                }
                h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
            }

            Bytes out(32);
            for (int i = 0; i < 8; ++i) {
                out[i * 4] = static_cast<uint8_t>(h[i] >> 24);
                out[i * 4 + 1] = static_cast<uint8_t>(h[i] >> 16);
                out[i * 4 + 2] = static_cast<uint8_t>(h[i] >> 8);
                out[i * 4 + 3] = static_cast<uint8_t>(h[i]);
            }
            return out;
        }

        // -----------------------------------------------------------------
        // SHA-1 (FIPS 180-4) — needed by TOTP (RFC 6238)
        // -----------------------------------------------------------------
        Bytes sha1(std::string_view data) {
            uint32_t h[5] = {0x67452301u, 0xefcdab89u, 0x98badcfeu, 0x10325476u, 0xc3d2e1f0u};
            uint64_t bit_len = static_cast<uint64_t>(data.size()) * 8ull;
            Bytes buf(data.begin(), data.end());
            buf.push_back(0x80);
            while (buf.size() % 64 != 56) buf.push_back(0x00);
            for (int i = 7; i >= 0; --i) buf.push_back(static_cast<uint8_t>((bit_len >> (i * 8)) & 0xFF));

            for (size_t chunk = 0; chunk < buf.size(); chunk += 64) {
                uint32_t w[80];
                for (int i = 0; i < 16; ++i) {
                    w[i] = (static_cast<uint32_t>(buf[chunk + i * 4]) << 24) |
                           (static_cast<uint32_t>(buf[chunk + i * 4 + 1]) << 16) |
                           (static_cast<uint32_t>(buf[chunk + i * 4 + 2]) << 8) |
                           (static_cast<uint32_t>(buf[chunk + i * 4 + 3]));
                }
                for (int i = 16; i < 80; ++i) w[i] = rotl32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

                uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
                for (int i = 0; i < 80; ++i) {
                    uint32_t f = 0, k = 0;
                    if (i < 20) { f = (b & c) | (~b & d); k = 0x5a827999u; }
                    else if (i < 40) { f = b ^ c ^ d; k = 0x6ed9eba1u; }
                    else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8f1bbcdcu; }
                    else { f = b ^ c ^ d; k = 0xca62c1d6u; }
                    const uint32_t temp = rotl32(a, 5) + f + e + k + w[i];
                    e = d; d = c; c = rotl32(b, 30); b = a; a = temp;
                }
                h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
            }
            Bytes out(20);
            for (int i = 0; i < 5; ++i) {
                out[i * 4] = static_cast<uint8_t>(h[i] >> 24);
                out[i * 4 + 1] = static_cast<uint8_t>(h[i] >> 16);
                out[i * 4 + 2] = static_cast<uint8_t>(h[i] >> 8);
                out[i * 4 + 3] = static_cast<uint8_t>(h[i]);
            }
            return out;
        }

        // -----------------------------------------------------------------
        // MD5 (RFC 1321). The K schedule is computed exactly as the RFC
        // defines it (floor(2^32 * |sin(i)|)) rather than transcribed.
        // -----------------------------------------------------------------
        Bytes md5(std::string_view data) {
            static const uint32_t s[64] = {
                7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
                5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
                4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
            static const auto kTable = [] {
                std::array<uint32_t, 64> k{};
                for (uint32_t i = 0; i < 64; ++i) {
                    k[i] = static_cast<uint32_t>(std::floor(std::abs(std::sin(static_cast<double>(i + 1))) * 4294967296.0));
                }
                return k;
            }();

            uint32_t a0 = 0x67452301u, b0 = 0xefcdab89u, c0 = 0x98badcfeu, d0 = 0x10325476u;
            uint64_t bit_len = static_cast<uint64_t>(data.size()) * 8ull;
            Bytes buf(data.begin(), data.end());
            buf.push_back(0x80);
            while (buf.size() % 64 != 56) buf.push_back(0x00);
            for (int i = 0; i < 8; ++i) buf.push_back(static_cast<uint8_t>((bit_len >> (i * 8)) & 0xFF));

            for (size_t chunk = 0; chunk < buf.size(); chunk += 64) {
                uint32_t m[16];
                for (int i = 0; i < 16; ++i) {
                    m[i] = static_cast<uint32_t>(buf[chunk + i * 4]) |
                           (static_cast<uint32_t>(buf[chunk + i * 4 + 1]) << 8) |
                           (static_cast<uint32_t>(buf[chunk + i * 4 + 2]) << 16) |
                           (static_cast<uint32_t>(buf[chunk + i * 4 + 3]) << 24);
                }
                uint32_t A = a0, B = b0, C = c0, D = d0;
                for (uint32_t i = 0; i < 64; ++i) {
                    uint32_t F = 0, g = 0;
                    if (i < 16) { F = (B & C) | (~B & D); g = i; }
                    else if (i < 32) { F = (D & B) | (~D & C); g = (5 * i + 1) % 16; }
                    else if (i < 48) { F = B ^ C ^ D; g = (3 * i + 5) % 16; }
                    else { F = C ^ (B | ~D); g = (7 * i) % 16; }
                    F = F + A + kTable[i] + m[g];
                    A = D; D = C; C = B;
                    B += rotl32(F, s[i]);
                }
                a0 += A; b0 += B; c0 += C; d0 += D;
            }
            Bytes out(16);
            const uint32_t words[4] = {a0, b0, c0, d0};
            for (int i = 0; i < 4; ++i) {
                out[i * 4] = static_cast<uint8_t>(words[i] & 0xFF);
                out[i * 4 + 1] = static_cast<uint8_t>((words[i] >> 8) & 0xFF);
                out[i * 4 + 2] = static_cast<uint8_t>((words[i] >> 16) & 0xFF);
                out[i * 4 + 3] = static_cast<uint8_t>((words[i] >> 24) & 0xFF);
            }
            return out;
        }

        [[maybe_unused]] size_t digestSize(DigestKind kind) {
            switch (kind) {
                case DigestKind::Sha256: return 32;
                case DigestKind::Sha1: return 20;
                case DigestKind::Md5: return 16;
                case DigestKind::Sha512: return 64;
            }
            return 0;
        }

        Bytes rawDigest(DigestKind kind, std::string_view data) {
            switch (kind) {
                case DigestKind::Sha256: return sha256(data);
                case DigestKind::Sha1: return sha1(data);
                case DigestKind::Md5: return md5(data);
                case DigestKind::Sha512: return {};
            }
            return {};
        }

    } // namespace

    std::string digest(DigestKind kind, std::string_view data) { return strOf(rawDigest(kind, data)); }

    std::string digestHex(DigestKind kind, std::string_view data) { return toHex(digest(kind, data)); }

    std::optional<DigestKind> kindFor(const std::string &algorithm) {
        std::string lowered = algorithm;
        std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (lowered == "sha256" || lowered == "sha-256" || lowered == "sha_256") return DigestKind::Sha256;
        if (lowered == "sha1" || lowered == "sha-1" || lowered == "sha_1") return DigestKind::Sha1;
        if (lowered == "md5") return DigestKind::Md5;
        return std::nullopt;
    }

    std::optional<std::string> digestHexByName(const std::string &algorithm, std::string_view data) {
        const auto kind = kindFor(algorithm);
        if (!kind.has_value()) return std::nullopt;
        return digestHex(*kind, data);
    }

    std::optional<std::string> hmac(DigestKind kind, std::string_view key, std::string_view data) {
        const size_t block = 64; // SHA-256/SHA-1/MD5 all use 64-byte blocks.
        Bytes key_bytes = bytesOf(key);
        if (key_bytes.size() > block) key_bytes = rawDigest(kind, svOf(key_bytes));
        key_bytes.resize(block, 0x00);
        Bytes inner_block(block), outer_block(block);
        for (size_t i = 0; i < block; ++i) {
            inner_block[i] = key_bytes[i] ^ 0x36;
            outer_block[i] = key_bytes[i] ^ 0x5c;
        }
        std::string inner(svOf(inner_block));
        inner.append(data);
        const Bytes inner_hash = rawDigest(kind, inner);

        std::string outer(svOf(outer_block));
        outer.append(svOf(inner_hash));
        return strOf(rawDigest(kind, outer));
    }

    std::optional<std::string> hmacHex(const std::string &algorithm, std::string_view key, std::string_view data) {
        const auto kind = kindFor(algorithm);
        if (!kind.has_value()) return std::nullopt;
        const auto raw = hmac(*kind, key, data);
        if (!raw.has_value()) return std::nullopt;
        return toHex(*raw);
    }

    std::string pbkdf2Sha256(std::string_view password, std::string_view salt, uint32_t iterations,
                             size_t derived_len) {
        std::string out;
        uint32_t block_index = 1;
        while (out.size() < derived_len) {
            std::string s = std::string(salt);
            s.push_back(static_cast<char>((block_index >> 24) & 0xFF));
            s.push_back(static_cast<char>((block_index >> 16) & 0xFF));
            s.push_back(static_cast<char>((block_index >> 8) & 0xFF));
            s.push_back(static_cast<char>(block_index & 0xFF));

            auto u = hmac(DigestKind::Sha256, password, s);
            if (!u.has_value()) break;
            Bytes t = bytesOf(*u);
            Bytes prev = t;
            for (uint32_t i = 1; i < iterations; ++i) {
                auto next = hmac(DigestKind::Sha256, password, svOf(prev));
                if (!next.has_value()) break;
                prev = bytesOf(*next);
                for (size_t j = 0; j < t.size(); ++j) t[j] ^= prev[j];
            }
            out.append(svOf(t));
            ++block_index;
        }
        out.resize(derived_len);
        return out;
    }

    bool timingSafeEquals(std::string_view a, std::string_view b) {
        if (a.size() != b.size()) return false;
        unsigned char diff = 0;
        for (size_t i = 0; i < a.size(); ++i) diff |= static_cast<unsigned char>(a[i] ^ b[i]);
        return diff == 0;
    }

    std::string randomBytes(size_t count) {
        // Seed once per call from the platform entropy source; `random_device`
        // on Linux reads getrandom(2), which is CSPRNG-backed.
        std::string out(count, '\0');
        static std::random_device rd;
        std::uniform_int_distribution<uint16_t> dist(0, 255);
        for (auto &c : out) c = static_cast<char>(dist(rd) & 0xFF);
        return out;
    }

    std::string randomHex(size_t byte_count) { return toHex(randomBytes(byte_count)); }

    std::string uuidV4() {
        std::string bytes = randomBytes(16);
        auto *b = reinterpret_cast<unsigned char *>(bytes.data());
        b[6] = static_cast<unsigned char>((b[6] & 0x0F) | 0x40); // version 4
        b[8] = static_cast<unsigned char>((b[8] & 0x3F) | 0x80); // variant 1
        const std::string hex = toHex(bytes);
        return hex.substr(0, 8) + "-" + hex.substr(8, 4) + "-" + hex.substr(12, 4) + "-" + hex.substr(16, 4) + "-" +
               hex.substr(20, 12);
    }

    std::string randomPin(size_t digits) {
        static std::random_device rd;
        std::mt19937_64 engine(rd());
        std::uniform_int_distribution<int> dist(0, 9);
        std::string pin;
        pin.reserve(digits);
        for (size_t i = 0; i < digits; ++i) pin.push_back(static_cast<char>('0' + dist(engine)));
        return pin;
    }

    std::optional<std::string> aesGcmEncrypt(std::string_view, std::string_view, std::string &) {
#if defined(SAPO_WITH_OPENSSL)
        // Handled in CryptoOpenssl.cpp when the dependency is enabled.
        return std::nullopt;
#else
        return std::nullopt;
#endif
    }

    std::optional<std::string> aesGcmDecrypt(std::string_view, std::string_view, std::string_view) {
        return std::nullopt;
    }

} // namespace sapo::util
