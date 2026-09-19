#include "util/Encoding.hpp"

#include <array>
#include <cctype>
#include <cstdint>

namespace sapo::util {

    namespace {
        constexpr char kBase64Alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        constexpr char kBase64UrlAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
        constexpr char kBase32Alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";

        std::string encodeBase(const std::string &input, const char *alphabet, bool padding) {
            std::string out;
            int val = 0, val_bits = -6;
            for (unsigned char c : input) {
                val = (val << 8) + c;
                val_bits += 8;
                while (val_bits >= 0) {
                    out.push_back(alphabet[(val >> val_bits) & 0x3F]);
                    val_bits -= 6;
                }
            }
            if (val_bits > -6) out.push_back(alphabet[((val << 8) >> (val_bits + 8)) & 0x3F]);
            if (padding) {
                while (out.size() % 4) out.push_back('=');
            }
            return out;
        }

        std::optional<std::string> decodeBase(const std::string &input, const char *alphabet) {
            std::array<int, 256> reverse{};
            reverse.fill(-1);
            for (int i = 0; i < 64; ++i) reverse[static_cast<unsigned char>(alphabet[i])] = i;

            std::string out;
            int val = 0, val_bits = -8;
            for (unsigned char c : input) {
                if (c == '=' ) break;
                if (std::isspace(c)) continue;
                const int p = reverse[c];
                if (p < 0) return std::nullopt;
                val = (val << 6) + p;
                val_bits += 6;
                if (val_bits >= 0) {
                    out.push_back(static_cast<char>((val >> val_bits) & 0xFF));
                    val_bits -= 8;
                }
            }
            return out;
        }
    } // namespace

    std::string base64Encode(std::string_view input) { return encodeBase(std::string(input), kBase64Alphabet, true); }

    std::optional<std::string> base64Decode(std::string_view input) {
        return decodeBase(std::string(input), kBase64Alphabet);
    }

    std::string base64UrlEncode(std::string_view input) {
        return encodeBase(std::string(input), kBase64UrlAlphabet, false);
    }

    std::optional<std::string> base64UrlDecode(std::string_view input) {
        std::string s(input);
        for (auto &c : s) {
            if (c == '-') c = '+';
            else if (c == '_') c = '/';
        }
        while (s.size() % 4 != 0) s.push_back('=');
        return decodeBase(s, kBase64Alphabet);
    }

    std::string toHex(std::string_view bytes) {
        static const char *digits = "0123456789abcdef";
        std::string out;
        out.reserve(bytes.size() * 2);
        for (unsigned char c : bytes) {
            out.push_back(digits[c >> 4]);
            out.push_back(digits[c & 0x0F]);
        }
        return out;
    }

    std::optional<std::string> fromHex(std::string_view hex) {
        auto nibble = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        if (hex.size() % 2 != 0) return std::nullopt;
        std::string out;
        out.reserve(hex.size() / 2);
        for (size_t i = 0; i < hex.size(); i += 2) {
            const int hi = nibble(hex[i]);
            const int lo = nibble(hex[i + 1]);
            if (hi < 0 || lo < 0) return std::nullopt;
            out.push_back(static_cast<char>((hi << 4) | lo));
        }
        return out;
    }

    std::string base32Encode(std::string_view input) {
        std::string out;
        int buffer = 0, bits_left = 0;
        for (unsigned char c : input) {
            buffer = (buffer << 8) | c;
            bits_left += 8;
            while (bits_left >= 5) {
                out.push_back(kBase32Alphabet[(buffer >> (bits_left - 5)) & 0x1F]);
                bits_left -= 5;
            }
        }
        if (bits_left > 0) out.push_back(kBase32Alphabet[(buffer << (5 - bits_left)) & 0x1F]);
        while (out.size() % 8 != 0) out.push_back('=');
        return out;
    }

    std::optional<std::string> base32Decode(std::string_view input) {
        auto lookup = [](char c) -> int {
            if (c >= 'A' && c <= 'Z') return c - 'A';
            if (c >= 'a' && c <= 'z') return c - 'a';
            if (c >= '2' && c <= '7') return c - '2' + 26;
            return -1;
        };
        std::string out;
        int buffer = 0, bits_left = 0;
        for (char ch : input) {
            if (ch == '=' || ch == ' ' || ch == '-') continue;
            const int v = lookup(ch);
            if (v < 0) return std::nullopt;
            buffer = (buffer << 5) | v;
            bits_left += 5;
            if (bits_left >= 8) {
                out.push_back(static_cast<char>((buffer >> (bits_left - 8)) & 0xFF));
                bits_left -= 8;
            }
        }
        return out;
    }

    std::string percentEncode(std::string_view input) {
        static const char *digits = "0123456789ABCDEF";
        std::string out;
        for (unsigned char c : input) {
            if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
                out.push_back(static_cast<char>(c));
            } else {
                out.push_back('%');
                out.push_back(digits[c >> 4]);
                out.push_back(digits[c & 0x0F]);
            }
        }
        return out;
    }

} // namespace sapo::util
