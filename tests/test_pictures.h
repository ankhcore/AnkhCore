// Shared by the picture tests (test_image_orient.cpp, test_thumbs.cpp): test pictures generated in memory with the WIC encoders.
// Pattern of every picture: green block in the top-left quarter-corner, red upper half, blue lower half.
#pragma once
#include "base.h"
#include "ui.h"
#include <string>
#include <vector>

namespace testpic {
using namespace ak;

inline std::vector<uint32_t> Pattern(int w, int h, bool block) {
    std::vector<uint32_t> p(static_cast<size_t>(w) * h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            p[static_cast<size_t>(y) * w + x] = (block && x < w / 4 && y < h / 4) ? 0xFF00FF00u : (y < h / 2 ? 0xFFFF0000u : 0xFF0000FFu);
    return p;
}

// Encodes the pattern as a file of the given WIC container format (PNG, BMP, JPEG, TIFF).
inline std::vector<uint8_t> EncodePixels(const GUID& container, int w, int h, const std::vector<uint32_t>& px, UINT orientation = 0) {
    std::vector<uint8_t> out;
    IWICImagingFactory* f = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)))) return out;
    IStream* st = nullptr; IWICBitmapEncoder* enc = nullptr; IWICBitmapFrameEncode* fr = nullptr; IPropertyBag2* pb = nullptr; IWICBitmap* bmp = nullptr;
    if (SUCCEEDED(CreateStreamOnHGlobal(nullptr, TRUE, &st)) && SUCCEEDED(f->CreateEncoder(container, nullptr, &enc)) && SUCCEEDED(enc->Initialize(st, WICBitmapEncoderNoCache)) &&
        SUCCEEDED(enc->CreateNewFrame(&fr, &pb)) && SUCCEEDED(fr->Initialize(pb)) && SUCCEEDED(fr->SetSize(w, h)) &&
        SUCCEEDED(f->CreateBitmapFromMemory(w, h, GUID_WICPixelFormat32bppBGRA, w * 4, static_cast<UINT>(px.size() * 4), reinterpret_cast<BYTE*>(const_cast<uint32_t*>(px.data())), &bmp))) {
        WICPixelFormatGUID pf = GUID_WICPixelFormat24bppBGR;
        fr->SetPixelFormat(&pf);
        if (orientation) {   // the picture's own orientation tag (JPEG: APP1 EXIF, TIFF: IFD)
            IWICMetadataQueryWriter* qw = nullptr;
            if (SUCCEEDED(fr->GetMetadataQueryWriter(&qw)) && qw) {
                PROPVARIANT pv; PropVariantInit(&pv); pv.vt = VT_UI2; pv.uiVal = static_cast<USHORT>(orientation);
                qw->SetMetadataByName(container == GUID_ContainerFormatJpeg ? L"/app1/ifd/{ushort=274}" : L"/ifd/{ushort=274}", &pv);
                qw->Release();
            }
        }
        IWICBitmapSource* src = bmp; IWICFormatConverter* cv = nullptr;
        if (pf != GUID_WICPixelFormat32bppBGRA && SUCCEEDED(f->CreateFormatConverter(&cv)) && SUCCEEDED(cv->Initialize(bmp, pf, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) src = cv;
        if (SUCCEEDED(fr->WriteSource(src, nullptr)) && SUCCEEDED(fr->Commit()) && SUCCEEDED(enc->Commit())) {
            HGLOBAL g = nullptr; GetHGlobalFromStream(st, &g);
            const SIZE_T n = GlobalSize(g); const void* m = GlobalLock(g);
            if (m) { out.assign(static_cast<const uint8_t*>(m), static_cast<const uint8_t*>(m) + n); GlobalUnlock(g); }
        }
        if (cv) cv->Release();
    }
    if (bmp) bmp->Release(); if (pb) pb->Release(); if (fr) fr->Release(); if (enc) enc->Release(); if (st) st->Release(); f->Release();
    return out;
}

inline std::vector<uint8_t> Encode(const GUID& container, int w, int h, bool block) { return EncodePixels(container, w, h, Pattern(w, h, block)); }

inline bool Near(uint32_t px, int r, int g, int b) {   // JPEG is lossy: flat areas stay within a few levels
    auto d = [](int a, int c) { return a > c ? a - c : c - a; };
    return d(static_cast<int>((px >> 16) & 255), r) < 60 && d(static_cast<int>((px >> 8) & 255), g) < 60 && d(static_cast<int>(px & 255), b) < 60;
}
inline bool Red(uint32_t p) { return Near(p, 255, 0, 0); }
inline bool Blue(uint32_t p) { return Near(p, 0, 0, 255); }
inline bool Green(uint32_t p) { return Near(p, 0, 255, 0); }

}  // namespace testpic
