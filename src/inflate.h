// Raw DEFLATE (RFC 1951) decoder with table lookups, plus the AnkhCore
// package format. Used for the setup payload and the stored icon packs.
#pragma once
#include <cstdint>
#include <cstring>
#include <vector>
#include <string>

namespace ak {

class Inflater {
public:
    // Decodes a complete raw deflate stream into out (out sized by caller).
    static bool Run(const uint8_t* in, size_t inSize, uint8_t* out, size_t outSize) {
        Inflater z(in, inSize, out, outSize);
        return z.Go();
    }

private:
    struct Huff {
        // fast table: 10-bit index -> (symbol << 4) | length, 0 = slow path
        uint16_t fast[1 << 10];
        uint16_t count[16];
        uint16_t symbol[320];
        bool Build(const uint8_t* lens, int n) {
            memset(count, 0, sizeof(count));
            for (int i = 0; i < n; ++i) count[lens[i]]++;
            count[0] = 0;
            int left = 1;
            for (int l = 1; l < 16; ++l) { left <<= 1; left -= count[l]; if (left < 0) return false; }
            uint16_t offs[16]; offs[1] = 0;
            for (int l = 1; l < 15; ++l) offs[l + 1] = offs[l] + count[l];
            for (int i = 0; i < n; ++i) if (lens[i]) symbol[offs[lens[i]]++] = static_cast<uint16_t>(i);
            memset(fast, 0, sizeof(fast));
            // canonical codes -> reversed bit patterns for the fast table
            int code = 0, idx = 0;
            for (int l = 1; l <= 10; ++l) {
                for (int k = 0; k < count[l]; ++k) {
                    const int sym = symbol[idx++];
                    int rev = 0;
                    for (int b = 0; b < l; ++b) if (code & (1 << b)) rev |= 1 << (l - 1 - b);
                    for (int fill = rev; fill < (1 << 10); fill += 1 << l) fast[fill] = static_cast<uint16_t>((sym << 4) | l);
                    ++code;
                }
                code <<= 1;
            }
            return true;
        }
    };
    const uint8_t* in; size_t inSize, inPos = 0;
    uint8_t* out; size_t outSize, outPos = 0;
    uint64_t bitbuf = 0; int bitcnt = 0; bool err = false;
    Inflater(const uint8_t* i, size_t is, uint8_t* o, size_t os) : in(i), inSize(is), out(o), outSize(os) {}

    int overrun = 0;
    void Need(int n) {
        while (bitcnt < n) {
            if (inPos < inSize) bitbuf |= static_cast<uint64_t>(in[inPos++]) << bitcnt;
            else if (++overrun > 8) { err = true; return; }
            bitcnt += 8;
        }
    }
    int Bits(int n) { if (!n) return 0; Need(n); const int v = static_cast<int>(bitbuf & ((1ull << n) - 1)); bitbuf >>= n; bitcnt -= n; return v; }
    int Decode(const Huff& h) {
        Need(15);
        const uint16_t f = h.fast[bitbuf & 1023];
        if (f) { const int l = f & 15; bitbuf >>= l; bitcnt -= l; return f >> 4; }
        // slow path, bit by bit (codes longer than 10 bits)
        int code = 0, first = 0, index = 0;
        for (int l = 1; l < 16; ++l) {
            code |= static_cast<int>(bitbuf & 1); bitbuf >>= 1; --bitcnt;
            const int c = h.count[l];
            if (code - c < first) return h.symbol[index + (code - first)];
            index += c; first += c; first <<= 1; code <<= 1;
        }
        err = true; return -1;
    }
    bool Stored() {
        bitbuf >>= bitcnt & 7; bitcnt -= bitcnt & 7;
        // return whole bytes in the bit buffer to the input
        while (bitcnt >= 8) { --inPos; bitcnt -= 8; }
        bitbuf = 0; bitcnt = 0;
        if (inPos + 4 > inSize) return false;
        const unsigned len = in[inPos] | (in[inPos + 1] << 8), nlen = in[inPos + 2] | (in[inPos + 3] << 8);
        inPos += 4;
        if (len != (~nlen & 0xffff) || inPos + len > inSize || outPos + len > outSize) return false;
        memcpy(out + outPos, in + inPos, len); inPos += len; outPos += len;
        return true;
    }
    bool Codes(const Huff& lit, const Huff& dist) {
        static const uint16_t lbase[29] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
        static const uint8_t lext[29] = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
        static const uint16_t dbase[30] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
        static const uint8_t dext[30] = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};
        for (;;) {
            int s = Decode(lit);
            if (s < 0 || err) return false;
            if (s < 256) { if (outPos >= outSize) return false; out[outPos++] = static_cast<uint8_t>(s); continue; }
            if (s == 256) return true;
            s -= 257;
            if (s >= 29) return false;
            const size_t len = lbase[s] + Bits(lext[s]);
            const int ds = Decode(dist);
            if (ds < 0 || ds >= 30) return false;
            const size_t d = dbase[ds] + Bits(dext[ds]);
            if (d > outPos || outPos + len > outSize) return false;
            uint8_t* o = out + outPos; const uint8_t* src = o - d;
            if (d >= len) memcpy(o, src, len); else for (size_t i = 0; i < len; ++i) o[i] = src[i];
            outPos += len;
        }
    }
    bool Fixed() {
        static Huff lit, dist; static bool built = false;
        if (!built) {
            uint8_t l[288];
            for (int i = 0; i < 144; ++i) l[i] = 8;
            for (int i = 144; i < 256; ++i) l[i] = 9;
            for (int i = 256; i < 280; ++i) l[i] = 7;
            for (int i = 280; i < 288; ++i) l[i] = 8;
            lit.Build(l, 288);
            uint8_t d[30]; for (auto& x : d) x = 5;
            dist.Build(d, 30);
            built = true;
        }
        return Codes(lit, dist);
    }
    bool Dynamic() {
        static const uint8_t order[19] = {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
        const int nlen = Bits(5) + 257, ndist = Bits(5) + 1, ncode = Bits(4) + 4;
        if (nlen > 286 || ndist > 30) return false;
        uint8_t lens[320] = {};
        for (int i = 0; i < ncode; ++i) lens[order[i]] = static_cast<uint8_t>(Bits(3));
        Huff lencode;
        if (!lencode.Build(lens, 19)) return false;
        int idx = 0;
        memset(lens, 0, sizeof(lens));
        while (idx < nlen + ndist) {
            int s = Decode(lencode);
            if (s < 0 || err) return false;
            if (s < 16) { lens[idx++] = static_cast<uint8_t>(s); continue; }
            int len = 0, rep;
            if (s == 16) { if (!idx) return false; len = lens[idx - 1]; rep = 3 + Bits(2); }
            else if (s == 17) rep = 3 + Bits(3);
            else rep = 11 + Bits(7);
            if (idx + rep > nlen + ndist) return false;
            while (rep--) lens[idx++] = static_cast<uint8_t>(len);
        }
        if (!lens[256]) return false;
        static thread_local Huff lit, dist;
        if (!lit.Build(lens, nlen)) return false;
        if (!dist.Build(lens + nlen, ndist)) return false;
        return Codes(lit, dist);
    }
    bool Go() {
        int last;
        do {
            last = Bits(1);
            const int type = Bits(2);
            bool ok = type == 0 ? Stored() : type == 1 ? Fixed() : type == 2 ? Dynamic() : false;
            if (!ok || err) return false;
        } while (!last);
        return outPos == outSize;
    }
};

// ---------------------------------------------------------------- package
// "ANKPK2\0\0" u32 count, then per entry:
//   u32 pathLen, path (UTF-8), u8 method (0 stored, 1 deflate), u64 rawSize,
//   u64 storedSize, data
struct PkEntry { std::wstring path; uint8_t method; uint64_t raw, stored; const uint8_t* data; };
inline bool ParsePackage(const uint8_t* d, size_t size, std::vector<PkEntry>& out) {
    if (size < 12 || memcmp(d, "ANKPK2\0\0", 8) != 0) return false;
    uint32_t n; memcpy(&n, d + 8, 4);
    size_t o = 12;
    for (uint32_t i = 0; i < n; ++i) {
        if (o + 4 > size) return false;
        uint32_t pl; memcpy(&pl, d + o, 4); o += 4;
        if (o + pl + 17 > size) return false;
        std::string p(reinterpret_cast<const char*>(d + o), pl); o += pl;
        PkEntry e; e.method = d[o++]; memcpy(&e.raw, d + o, 8); o += 8; memcpy(&e.stored, d + o, 8); o += 8;
        if (o + e.stored > size) return false;
        e.data = d + o; o += static_cast<size_t>(e.stored);
        const int wn = MultiByteToWideChar(CP_UTF8, 0, p.data(), static_cast<int>(p.size()), nullptr, 0);
        e.path.resize(wn); MultiByteToWideChar(CP_UTF8, 0, p.data(), static_cast<int>(p.size()), e.path.data(), wn);
        out.push_back(e);
    }
    return true;
}
inline bool Unpack(const PkEntry& e, std::vector<uint8_t>& out) {
    out.resize(static_cast<size_t>(e.raw));
    if (e.method == 0) { if (e.raw != e.stored) return false; memcpy(out.data(), e.data, out.size()); return true; }
    return Inflater::Run(e.data, static_cast<size_t>(e.stored), out.data(), out.size());
}
// Single compressed file: u64 rawSize + raw deflate.
inline bool LoadDz(const std::vector<uint8_t>& file, std::vector<uint8_t>& out) {
    if (file.size() < 8) return false;
    uint64_t raw; memcpy(&raw, file.data(), 8);
    out.resize(static_cast<size_t>(raw));
    return Inflater::Run(file.data() + 8, file.size() - 8, out.data(), out.size());
}

}  // namespace ak
