// PE resource rebuilder — replaces a module's icon resources (RT_ICON,
// RT_GROUP_ICON) with a pack's, keeping every other resource. Used only when
// Windows' own UpdateResource refuses a file (error 50 ERROR_NOT_SUPPORTED on
// large system files such as imageres.dll.mun). The new resource directory is
// written as a fresh section appended to the file, so nothing else moves.
// The caller verifies the result before it is swapped in.
//
// Pure byte manipulation; no Windows headers, so it can be unit-tested on any
// platform. Works on PE32 and PE32+.
#pragma once
#include <cstdint>
#include <cstring>
#include <vector>
#include <map>
#include <string>
#include <algorithm>

namespace pe {

struct Res { uint16_t type; bool nameIsId; uint16_t nameId; std::u16string name; uint16_t lang; std::vector<uint8_t> data; };

// little-endian helpers
inline uint16_t R16(const uint8_t* p) { return p[0] | (p[1] << 8); }
inline uint32_t R32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t(p[3]) << 24); }
inline void W16(uint8_t* p, uint16_t v) { p[0] = v & 0xff; p[1] = v >> 8; }
inline void W32(uint8_t* p, uint32_t v) { p[0] = v & 0xff; p[1] = (v >> 8) & 0xff; p[2] = (v >> 16) & 0xff; p[3] = (v >> 24) & 0xff; }

struct PE {
    const uint8_t* d = nullptr; size_t n = 0;
    uint32_t peOff = 0; bool plus = false;
    uint32_t secOff = 0; uint16_t numSec = 0; uint16_t optSize = 0;
    uint32_t fileAlign = 0x200, secAlign = 0x1000, sizeOfImage = 0;
    uint32_t resRva = 0, resSize = 0;        // resource data directory
    uint32_t ddOff = 0; uint16_t ddCount = 0;

    bool Parse(const uint8_t* data, size_t size) {
        d = data; n = size;
        if (n < 0x40 || R16(d) != 0x5A4D) return false;
        peOff = R32(d + 0x3c);
        if (peOff + 0x18 > n || R32(d + peOff) != 0x00004550) return false;
        const uint8_t* fh = d + peOff + 4;
        numSec = R16(fh + 2);
        optSize = R16(fh + 16);
        const uint8_t* oh = fh + 20;
        const uint16_t magic = R16(oh);
        plus = magic == 0x20b;
        if (!plus && magic != 0x10b) return false;
        fileAlign = R32(oh + 36);
        secAlign = R32(oh + 32);
        sizeOfImage = R32(oh + 56);
        const uint32_t ddBase = plus ? 112 : 96;
        ddCount = R32(oh + (plus ? 108 : 92));
        ddOff = peOff + 4 + 20 + ddBase;       // file offset of data directory array
        if (ddCount < 3) return false;
        resRva = R32(d + ddOff + 2 * 8);
        resSize = R32(d + ddOff + 2 * 8 + 4);
        secOff = peOff + 4 + 20 + optSize;
        if (secOff + numSec * 40 > n) return false;
        return true;
    }
    // section containing an RVA -> file offset
    uint32_t RvaToOff(uint32_t rva) const {
        for (uint16_t i = 0; i < numSec; ++i) {
            const uint8_t* s = d + secOff + i * 40;
            const uint32_t va = R32(s + 12), vs = R32(s + 8), raw = R32(s + 20), rs = R32(s + 16);
            const uint32_t span = std::max(vs, rs);
            if (rva >= va && rva < va + span) return raw + (rva - va);
        }
        return 0;
    }
};

// ---- parse the resource tree into a flat list ----
inline void ParseLevel(const PE& pe, uint32_t dirRva, int level, uint16_t type, bool nameIsId, uint16_t nameId,
                       const std::u16string& name, std::vector<Res>& out) {
    const uint32_t off = pe.RvaToOff(dirRva);
    if (!off || off + 16 > pe.n) return;
    const uint8_t* dir = pe.d + off;
    const uint16_t nNamed = R16(dir + 12), nId = R16(dir + 14);
    const int total = nNamed + nId;
    for (int i = 0; i < total; ++i) {
        const uint8_t* e = dir + 16 + i * 8;
        const uint32_t nameField = R32(e), offField = R32(e + 4);
        bool isId = !(nameField & 0x80000000);
        uint16_t idVal = nameField & 0xffff;
        std::u16string strVal;
        if (!isId) {
            const uint32_t so = pe.RvaToOff(pe.resRva + (nameField & 0x7fffffff));
            if (so && so + 2 <= pe.n) { const uint16_t len = R16(pe.d + so); for (uint16_t k = 0; k < len; ++k) strVal.push_back(R16(pe.d + so + 2 + k * 2)); }
        }
        if (offField & 0x80000000) {   // subdirectory
            const uint32_t subRva = pe.resRva + (offField & 0x7fffffff);
            if (level == 0) ParseLevel(pe, subRva, 1, idVal, isId, 0, u"", out);               // type level (always id)
            else if (level == 1) ParseLevel(pe, subRva, 2, type, isId, idVal, strVal, out);    // name level
        } else {                        // data entry (language level)
            const uint32_t deOff = pe.RvaToOff(pe.resRva + offField);
            if (!deOff || deOff + 16 > pe.n) continue;
            const uint32_t dataRva = R32(pe.d + deOff), dataSize = R32(pe.d + deOff + 4);
            const uint32_t dataOff = pe.RvaToOff(dataRva);
            if (!dataOff || dataOff + dataSize > pe.n) continue;
            Res r; r.type = type; r.nameIsId = nameIsId; r.nameId = nameId; r.name = name; r.lang = idVal;
            r.data.assign(pe.d + dataOff, pe.d + dataOff + dataSize);
            out.push_back(r);
        }
    }
}
inline bool ParseResources(const PE& pe, std::vector<Res>& out) {
    if (!pe.resRva) return false;
    ParseLevel(pe, pe.resRva, 0, 0, true, 0, u"", out);
    return true;
}

// ---- serialize a resource directory blob at a given RVA ----
// Groups resources by type -> name -> lang. Named entries get a string pool.
struct Builder {
    uint32_t baseRva;
    std::vector<uint8_t> blob;
    explicit Builder(uint32_t rva) : baseRva(rva) {}

    struct Lang { uint16_t lang; const std::vector<uint8_t>* data; };
    struct Name { bool isId; uint16_t id; std::u16string str; std::vector<Lang> langs; };
    struct Type { uint16_t id; std::vector<Name> names; };

    static void SortBuild(std::vector<Res>& res, std::vector<Type>& types) {
        std::map<uint16_t, std::map<std::pair<int, std::u16string>, std::map<uint16_t, const std::vector<uint8_t>*>>> m;
        // keep id names keyed by (0,id-as-string-less) ... use (isId?0:1, key)
        std::map<uint16_t, std::map<std::u16string, Name>> tmp;
        for (auto& r : res) {
            Name& nm = tmp[r.type][r.nameIsId ? u"#" + std::u16string(1, (char16_t)r.nameId) + u"_" + std::u16string(1, (char16_t)(r.nameId >> 8)) : u"$" + r.name];
            nm.isId = r.nameIsId; nm.id = r.nameId; nm.str = r.name;
            bool replaced = false;
            for (auto& lg : nm.langs) if (lg.lang == r.lang) { lg.data = &r.data; replaced = true; break; }
            if (!replaced) nm.langs.push_back({r.lang, &r.data});
        }
        for (auto& tk : tmp) {
            Type t; t.id = tk.first;
            for (auto& nk : tk.second) {
                Name nm = nk.second;
                std::sort(nm.langs.begin(), nm.langs.end(), [](const Lang& a, const Lang& b) { return a.lang < b.lang; });
                t.names.push_back(nm);
            }
            // ID names ascending, named entries first (PE requires named before id, sorted)
            std::sort(t.names.begin(), t.names.end(), [](const Name& a, const Name& b) {
                if (a.isId != b.isId) return !a.isId;                 // named first
                if (!a.isId) return a.str < b.str;
                return a.id < b.id;
            });
            types.push_back(t);
        }
        std::sort(types.begin(), types.end(), [](const Type& a, const Type& b) { return a.id < b.id; });
    }

    // Build the blob. Layout: all directory tables, then data-entry descriptors,
    // then the string pool, then the raw resource data (4-byte aligned).
    std::vector<uint8_t> Build(std::vector<Res>& res) {
        std::vector<Type> types;
        SortBuild(res, types);
        // sizes
        auto dirSize = [](size_t entries) { return 16 + entries * 8; };
        size_t sz = dirSize(types.size());
        for (auto& t : types) { sz += dirSize(t.names.size()); for (auto& nm : t.names) sz += dirSize(nm.langs.size()); }
        const size_t dirEnd = sz;
        size_t nData = 0; for (auto& t : types) for (auto& nm : t.names) nData += nm.langs.size();
        sz += nData * 16;                        // data entry descriptors
        const size_t dataDescEnd = sz;
        // string pool
        size_t strPool = sz;
        for (auto& t : types) for (auto& nm : t.names) if (!nm.isId) sz += 2 + nm.str.size() * 2;
        sz = (sz + 3) & ~size_t(3);
        const size_t dataStart = sz;
        for (auto& t : types) for (auto& nm : t.names) for (auto& lg : nm.langs) sz += (lg.data->size() + 3) & ~size_t(3);

        blob.assign(sz, 0);
        // writer cursors
        size_t dirCur = 0;                       // directory tables written front to back
        size_t dataDescCur = dirEnd;             // data entry descriptors
        size_t strCur = dataDescEnd;             // strings
        size_t dataCur = dataStart;              // raw data

        // We must write directories in order: root, then each type's name dir,
        // then each name's lang dir. Compute offsets as we go using a queue.
        // Root directory:
        auto writeDirHeader = [&](size_t at, uint16_t named, uint16_t ids) {
            W32(&blob[at], 0); W32(&blob[at + 4], 0); W16(&blob[at + 8], 0); W16(&blob[at + 10], 0);
            W16(&blob[at + 12], named); W16(&blob[at + 14], ids);
        };
        // Pre-assign directory offsets.
        size_t rootAt = 0;
        size_t cursor = dirSize(types.size());
        std::vector<size_t> typeDirAt(types.size());
        std::vector<std::vector<size_t>> nameDirAt(types.size());
        for (size_t ti = 0; ti < types.size(); ++ti) {
            typeDirAt[ti] = cursor; cursor += dirSize(types[ti].names.size());
        }
        for (size_t ti = 0; ti < types.size(); ++ti) {
            nameDirAt[ti].resize(types[ti].names.size());
            for (size_t ni = 0; ni < types[ti].names.size(); ++ni) { nameDirAt[ti][ni] = cursor; cursor += dirSize(types[ti].names[ni].langs.size()); }
        }
        (void)dirCur;
        // root
        uint16_t rootNamed = 0, rootIds = (uint16_t)types.size();
        writeDirHeader(rootAt, rootNamed, rootIds);
        for (size_t ti = 0; ti < types.size(); ++ti) {
            uint8_t* e = &blob[rootAt + 16 + ti * 8];
            W32(e, types[ti].id);
            W32(e + 4, (uint32_t)typeDirAt[ti] | 0x80000000);
        }
        // type dirs
        for (size_t ti = 0; ti < types.size(); ++ti) {
            auto& t = types[ti];
            uint16_t named = 0; for (auto& nm : t.names) if (!nm.isId) named++;
            writeDirHeader(typeDirAt[ti], named, (uint16_t)(t.names.size() - named));
            for (size_t ni = 0; ni < t.names.size(); ++ni) {
                uint8_t* e = &blob[typeDirAt[ti] + 16 + ni * 8];
                if (t.names[ni].isId) W32(e, t.names[ni].id);
                else {
                    // write string to pool, reference it
                    const size_t sAt = strCur;
                    W16(&blob[sAt], (uint16_t)t.names[ni].str.size());
                    for (size_t k = 0; k < t.names[ni].str.size(); ++k) W16(&blob[sAt + 2 + k * 2], t.names[ni].str[k]);
                    strCur += 2 + t.names[ni].str.size() * 2;
                    W32(e, (uint32_t)sAt | 0x80000000);
                }
                W32(e + 4, (uint32_t)nameDirAt[ti][ni] | 0x80000000);
            }
        }
        // name dirs (lang level) + data descriptors + raw data
        for (size_t ti = 0; ti < types.size(); ++ti) {
            for (size_t ni = 0; ni < types[ti].names.size(); ++ni) {
                auto& nm = types[ti].names[ni];
                writeDirHeader(nameDirAt[ti][ni], 0, (uint16_t)nm.langs.size());
                for (size_t li = 0; li < nm.langs.size(); ++li) {
                    uint8_t* e = &blob[nameDirAt[ti][ni] + 16 + li * 8];
                    W32(e, nm.langs[li].lang);
                    const size_t descAt = dataDescCur; dataDescCur += 16;
                    W32(e + 4, (uint32_t)descAt);           // high bit clear -> data entry
                    // raw data
                    const size_t at = dataCur;
                    memcpy(&blob[at], nm.langs[li].data->data(), nm.langs[li].data->size());
                    dataCur += (nm.langs[li].data->size() + 3) & ~size_t(3);
                    // data entry descriptor: RVA, Size, CodePage, Reserved
                    W32(&blob[descAt], baseRva + (uint32_t)at);
                    W32(&blob[descAt + 4], (uint32_t)nm.langs[li].data->size());
                    W32(&blob[descAt + 8], 0);
                    W32(&blob[descAt + 12], 0);
                }
            }
        }
        (void)strPool;
        return blob;
    }
};

// Reason codes so the caller can log exactly what happened.
enum RebuildErr {
    RB_OK = 0, RB_PARSE = 1, RB_BUILD = 2, RB_NOROOM = 3, RB_NOSEC = 4
};

// Replace RT_ICON (3) and RT_GROUP_ICON (14) with newIcons; keep the rest.
// Returns the rebuilt file bytes, or empty on failure (err set to a RebuildErr).
//
// Strategy: we ALWAYS reuse the module's existing .rsrc section header instead
// of appending a new one. System resource files such as imageres.dll.mun pack
// their PE headers tightly, leaving no room for an extra section header — that
// was why appending failed. Reusing the header needs no extra room: we point
// the existing resource section at a freshly built resource blob placed at the
// end of the file, and abandon the old resource bytes (harmless). Every target
// file has icons, so it always has a resource section to reuse.
inline std::vector<uint8_t> ReplaceIconsEx(const uint8_t* file, size_t size, const std::vector<Res>& newIcons, int& err) {
    err = RB_PARSE;
    PE pe;
    if (!pe.Parse(file, size)) return {};
    std::vector<Res> res;
    ParseResources(pe, res);   // may be empty (no existing resources) — still fine
    // drop existing icon resources
    std::vector<Res> kept;
    for (auto& r : res) if (r.type != 3 && r.type != 14) kept.push_back(r);
    for (auto& r : newIcons) kept.push_back(r);

    // find the section index that currently holds the resource directory,
    // and the highest virtual address end across all sections.
    int rsrcSec = -1; uint32_t rsrcVa = 0; uint32_t maxVaEnd = 0; bool rsrcIsLast = false;
    for (uint16_t i = 0; i < pe.numSec; ++i) {
        const uint8_t* s = file + pe.secOff + i * 40;
        const uint32_t va = R32(s + 12), vs = R32(s + 8), rs = R32(s + 16);
        const uint32_t span = std::max(vs, rs);
        const uint32_t vaEnd = (va + span + pe.secAlign - 1) & ~(pe.secAlign - 1);
        if (vaEnd > maxVaEnd) maxVaEnd = vaEnd;
        if (pe.resRva && pe.resRva >= va && pe.resRva < va + span) { rsrcSec = i; rsrcVa = va; }
    }
    if (rsrcSec >= 0) {
        // is the resource section the one with the highest VA (nothing after it)?
        rsrcIsLast = true;
        for (uint16_t i = 0; i < pe.numSec; ++i) {
            if ((int)i == rsrcSec) continue;
            const uint32_t va = R32(file + pe.secOff + i * 40 + 12);
            if (va > rsrcVa) { rsrcIsLast = false; break; }
        }
    }

    // Choose where the rebuilt resources live in virtual space, keeping the
    // image VALID (contiguous — no hole a SEC_IMAGE map would reject):
    //  * resource section is last  -> grow it IN PLACE at its own VA (best; this
    //    is the usual case for .mun files and needs no header room);
    //  * otherwise                 -> append a brand-new section past the image
    //    end and leave the old resource section in place (needs 1 header slot).
    uint32_t newRva;
    bool growInPlace = (rsrcSec >= 0 && rsrcIsLast);
    if (growInPlace) newRva = rsrcVa;
    else             newRva = (maxVaEnd ? maxVaEnd : pe.sizeOfImage + pe.secAlign - 1) & ~(pe.secAlign - 1);

    Builder b(newRva);
    std::vector<uint8_t> blob = b.Build(kept);
    if (blob.empty()) { err = RB_BUILD; return {}; }

    // raw placement at end of file, file-aligned
    const uint32_t rawPtr = (uint32_t)((size + pe.fileAlign - 1) & ~(size_t)(pe.fileAlign - 1));
    const uint32_t rawSize = (uint32_t)((blob.size() + pe.fileAlign - 1) & ~(size_t)(pe.fileAlign - 1));

    std::vector<uint8_t> out(file, file + size);
    out.resize((size_t)rawPtr + rawSize, 0);
    memcpy(&out[rawPtr], blob.data(), blob.size());

    uint8_t* sh;
    if (growInPlace) {
        // reuse the last (resource) section header, same VA, new size/location
        sh = &out[pe.secOff + rsrcSec * 40];
        memcpy(sh, ".rsrc\0\0", 7);
    } else {
        // append a new section header (keeps old resource section intact -> no gap)
        const uint32_t firstSecRaw = [&] { uint32_t m = 0xffffffff; for (uint16_t i = 0; i < pe.numSec; ++i) { uint32_t r = R32(file + pe.secOff + i * 40 + 20); if (r && r < m) m = r; } return m; }();
        if (pe.secOff + (pe.numSec + 1) * 40 > firstSecRaw) { err = RB_NOROOM; return {}; }
        sh = &out[pe.secOff + pe.numSec * 40];
        memset(sh, 0, 40);
        memcpy(sh, ".ankh\0\0", 7);
        W16(&out[pe.peOff + 4 + 2], pe.numSec + 1);   // bump section count
    }
    W32(sh + 8, (uint32_t)blob.size());              // VirtualSize
    W32(sh + 12, newRva);                            // VirtualAddress
    W32(sh + 16, rawSize);                           // SizeOfRawData
    W32(sh + 20, rawPtr);                            // PointerToRawData
    W32(sh + 24, 0); W32(sh + 28, 0); W32(sh + 32, 0);// reloc/line ptrs & counts
    W32(sh + 36, 0x40000040);                        // INITIALIZED_DATA | MEM_READ

    // optional header: SizeOfImage
    const uint8_t* oh0 = file + pe.peOff + 4 + 20;
    const uint32_t newSizeOfImage = newRva + ((uint32_t(blob.size()) + pe.secAlign - 1) & ~(pe.secAlign - 1));
    W32(&out[(oh0 - file) + 56], newSizeOfImage);
    // data directory [2] (resource) -> rebuilt blob
    W32(&out[pe.ddOff + 2 * 8], newRva);
    W32(&out[pe.ddOff + 2 * 8 + 4], (uint32_t)blob.size());
    // zero the PE checksum (not validated for resource modules loaded as data)
    W32(&out[(oh0 - file) + 64], 0);
    err = RB_OK;
    return out;
}

// Backwards-compatible wrapper.
inline std::vector<uint8_t> ReplaceIcons(const uint8_t* file, size_t size, const std::vector<Res>& newIcons) {
    int err = 0; return ReplaceIconsEx(file, size, newIcons, err);
}

// Verify a rebuilt file: parses and confirms it has RT_GROUP_ICON entries.
inline bool VerifyIcons(const std::vector<uint8_t>& file, int& groupIcons, int& icons) {
    PE pe; groupIcons = icons = 0;
    if (!pe.Parse(file.data(), file.size())) return false;
    std::vector<Res> res;
    if (!ParseResources(pe, res)) return false;
    for (auto& r : res) { if (r.type == 14) groupIcons++; if (r.type == 3) icons++; }
    return groupIcons > 0 && icons > 0;
}

}  // namespace pe
