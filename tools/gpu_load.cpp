// AnkhCore test tool: a sustained, bounded GPU workload on one chosen adapter, to verify GPU utilization
// readings against real load. It renders an expensive pixel shader to an off-screen texture; it shows no
// window, changes no setting and exits by itself. Not part of the AnkhCore build or installer.
//   gpu_load.exe --list                      adapters with their LUIDs
//   gpu_load.exe <adapter index> <seconds> [duty %]
// Build: tools\build_gpu_load.cmd
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <d3dcompiler.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

static const char* kVs = "float4 main(uint id : SV_VertexID) : SV_Position { float2 p = float2((id << 1) & 2, id & 2); return float4(p * 2 - 1, 0, 1); }";
static const char* kPs =
    "float4 main(float4 pos : SV_Position) : SV_Target { float2 z = 0, c = pos.xy / 1024.0 - float2(1.3, 1.0); float n = 0;"
    " for (int i = 0; i < 400; ++i) { z = float2(z.x * z.x - z.y * z.y, 2 * z.x * z.y) + c; n += dot(z, z) < 4 ? 1 : 0; } return float4(n / 400, 0, 0, 1); }";

int wmain(int argc, wchar_t** argv) {
    IDXGIFactory1* f = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&f)))) { wprintf(L"DXGI unavailable\n"); return 2; }
    if (argc < 3 || !wcscmp(argv[1], L"--list")) {
        IDXGIAdapter1* a = nullptr;
        for (UINT i = 0; f->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; ++i) {
            DXGI_ADAPTER_DESC1 d{}; a->GetDesc1(&d);
            wprintf(L"%u | luid 0x%08x_0x%08x | software %d | %ls\n", i, static_cast<unsigned>(d.AdapterLuid.HighPart), static_cast<unsigned>(d.AdapterLuid.LowPart), (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) ? 1 : 0, d.Description);
            a->Release();
        }
        return argc < 3 && argc != 2 ? 1 : 0;
    }
    const UINT index = static_cast<UINT>(_wtoi(argv[1])); const int seconds = min(120, max(1, _wtoi(argv[2]))); const int duty = argc > 3 ? min(100, max(5, _wtoi(argv[3]))) : 100;
    IDXGIAdapter1* ad = nullptr;
    if (f->EnumAdapters1(index, &ad) == DXGI_ERROR_NOT_FOUND) { wprintf(L"no adapter %u\n", index); return 3; }
    DXGI_ADAPTER_DESC1 d{}; ad->GetDesc1(&d);
    ID3D11Device* dev = nullptr; ID3D11DeviceContext* ctx = nullptr;
    if (FAILED(D3D11CreateDevice(ad, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr, &ctx))) { wprintf(L"no Direct3D 11 device on %ls\n", d.Description); return 4; }
    ID3DBlob *vb = nullptr, *pb = nullptr, *err = nullptr;
    if (FAILED(D3DCompile(kVs, strlen(kVs), nullptr, nullptr, nullptr, "main", "vs_5_0", 0, 0, &vb, &err)) || FAILED(D3DCompile(kPs, strlen(kPs), nullptr, nullptr, nullptr, "main", "ps_5_0", 0, 0, &pb, &err))) {
        wprintf(L"shader compile failed: %hs\n", err ? static_cast<const char*>(err->GetBufferPointer()) : "?"); return 5;
    }
    ID3D11VertexShader* vs = nullptr; ID3D11PixelShader* ps = nullptr;
    dev->CreateVertexShader(vb->GetBufferPointer(), vb->GetBufferSize(), nullptr, &vs);
    dev->CreatePixelShader(pb->GetBufferPointer(), pb->GetBufferSize(), nullptr, &ps);
    D3D11_TEXTURE2D_DESC td{}; td.Width = td.Height = 2048; td.MipLevels = td.ArraySize = 1; td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1; td.BindFlags = D3D11_BIND_RENDER_TARGET;
    ID3D11Texture2D* tex = nullptr; ID3D11RenderTargetView* rtv = nullptr;
    if (FAILED(dev->CreateTexture2D(&td, nullptr, &tex)) || FAILED(dev->CreateRenderTargetView(tex, nullptr, &rtv))) { wprintf(L"render target failed\n"); return 6; }
    D3D11_QUERY_DESC qd{D3D11_QUERY_EVENT, 0}; ID3D11Query* q = nullptr; dev->CreateQuery(&qd, &q);
    D3D11_VIEWPORT vp{0, 0, 2048, 2048, 0, 1};
    ctx->OMSetRenderTargets(1, &rtv, nullptr); ctx->RSSetViewports(1, &vp);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST); ctx->VSSetShader(vs, nullptr, 0); ctx->PSSetShader(ps, nullptr, 0);
    wprintf(L"loading %ls (luid 0x%08x_0x%08x) for %d s at %d %% duty\n", d.Description, static_cast<unsigned>(d.AdapterLuid.HighPart), static_cast<unsigned>(d.AdapterLuid.LowPart), seconds, duty);
    fflush(stdout);
    const ULONGLONG end = GetTickCount64() + static_cast<ULONGLONG>(seconds) * 1000; unsigned long long frames = 0;
    while (GetTickCount64() < end) {
        const ULONGLONG slice = GetTickCount64();
        while (GetTickCount64() - slice < static_cast<ULONGLONG>(duty)) {   // busy for `duty` ms of every 100 ms
            for (int k = 0; k < 32; ++k) ctx->Draw(3, 0);   // enough work per round trip to keep a fast GPU busy
            ctx->End(q); ctx->Flush();
            BOOL done = FALSE; while (ctx->GetData(q, &done, sizeof(done), 0) != S_OK && GetTickCount64() < end) Sleep(0);   // wait for the GPU: no unbounded queue
            ++frames;
        }
        if (duty < 100) Sleep(100 - duty);
    }
    wprintf(L"done: %llu frames\n", frames);
    return 0;
}
