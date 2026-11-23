// AnkhCore 2.9.5: the real Windows Update provider (Windows Update Agent COM API, wuapi.h). Only app.cpp includes this file; tests and the isolated TEST build use
// WuFakeProvider (eng_wua.h). What this provider can do: search (read-only) and download one update. It has no code path that installs, restarts, changes a policy or
// touches a service: there is no such call in this file.
#pragma once
#include "eng_wua.h"
#include <wuapi.h>
#include <wrl/client.h>
#include <oleauto.h>

namespace ak {

struct WuComScope {
    bool init = false;
    WuComScope() { const HRESULT h = CoInitializeEx(nullptr, COINIT_MULTITHREADED); init = (h == S_OK || h == S_FALSE); }   // RPC_E_CHANGED_MODE: the thread already has an apartment: usable as it is
    ~WuComScope() { if (init) CoUninitialize(); }
};
struct WuBstr {
    BSTR b = nullptr;
    explicit WuBstr(const std::wstring& s) : b(SysAllocStringLen(s.data(), static_cast<UINT>(s.size()))) {}
    ~WuBstr() { SysFreeString(b); }
    WuBstr(const WuBstr&) = delete; WuBstr& operator=(const WuBstr&) = delete;
};
inline std::wstring WuTake(BSTR b) { std::wstring s = b ? std::wstring(b, SysStringLen(b)) : std::wstring(); SysFreeString(b); return s; }

// Does nothing: Windows wants callback objects for an asynchronous download, but all progress is read by polling the job, so no callback ever touches application state.
struct WuNoopCallback : IDownloadProgressChangedCallback, IDownloadCompletedCallback {
    volatile LONG refs = 1;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** pp) override {
        if (!pp) return E_POINTER;
        if (iid == IID_IUnknown || iid == __uuidof(IDownloadProgressChangedCallback)) *pp = static_cast<IDownloadProgressChangedCallback*>(this);
        else if (iid == __uuidof(IDownloadCompletedCallback)) *pp = static_cast<IDownloadCompletedCallback*>(this);
        else { *pp = nullptr; return E_NOINTERFACE; }
        AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&refs)); }
    ULONG STDMETHODCALLTYPE Release() override { const LONG n = InterlockedDecrement(&refs); if (n == 0) delete this; return static_cast<ULONG>(n); }
    HRESULT STDMETHODCALLTYPE Invoke(IDownloadJob*, IDownloadProgressChangedCallbackArgs*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE Invoke(IDownloadJob*, IDownloadCompletedCallbackArgs*) override { return S_OK; }
};

class WuaProvider : public WuProvider {
public:
    WuSearchResult Search() override {
        WuSearchResult r; WuComScope com;
        Microsoft::WRL::ComPtr<IUpdateSession> sess;
        HRESULT hr = CoCreateInstance(__uuidof(UpdateSession), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&sess));
        if (FAILED(hr)) { r.err = WuClassifyHr(hr); return r; }
        { WuBstr app(L"AnkhCore"); sess->put_ClientApplicationID(app.b); }
        std::vector<WuUpdate> avail;
        if (FAILED(hr = Run(sess.Get(), L"IsInstalled=0 and IsHidden=0", true, 200, avail))) { r.err = WuClassifyHr(hr); return r; }   // the one list that needs the network and decides success
        r.ok = true; r.updates = std::move(avail);
        std::wstring note;
        std::vector<WuUpdate> hidden, installed;
        if (SUCCEEDED(Run(sess.Get(), L"IsInstalled=0 and IsHidden=1", false, 50, hidden))) for (auto& u : hidden) r.updates.push_back(std::move(u)); else note += L"The hidden list could not be read. ";
        if (SUCCEEDED(Run(sess.Get(), L"IsInstalled=1 and IsHidden=0", false, 25, installed))) { if (!installed.empty()) note += L"Installed: the first " + std::to_wstring(installed.size()) + L" Windows reports."; for (auto& u : installed) r.updates.push_back(std::move(u)); }
        else note += L"The installed list could not be read.";
        r.note = note;
        return r;
    }

    WuDlOutcome DownloadOne(const WuUpdate& u, const std::function<void(int)>& progress, const std::function<bool()>& cancel) override {
        WuDlOutcome o; WuComScope com;
        Microsoft::WRL::ComPtr<IUpdateSession> sess;
        HRESULT hr = CoCreateInstance(__uuidof(UpdateSession), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&sess));
        if (FAILED(hr)) { o.hr = hr; o.text = WuClassifyHr(hr).text; return o; }
        { WuBstr app(L"AnkhCore"); sess->put_ClientApplicationID(app.b); }
        // a fresh handle on exactly this update (the one the person chose)
        Microsoft::WRL::ComPtr<IUpdate> up;
        hr = Find(sess.Get(), u.id, true, up);
        if (FAILED(hr)) { o.hr = hr; o.text = WuClassifyHr(hr).text; return o; }
        if (!up) { o.hr = 0; o.text = L"Windows no longer offers this update. Check again."; return o; }
        VARIANT_BOOL eula = VARIANT_TRUE; up->get_EulaAccepted(&eula);
        if (!eula) { o.text = L"Its licence terms have not been accepted. Accept them in Windows Update first: AnkhCore never accepts licence terms for you."; return o; }
        Microsoft::WRL::ComPtr<IUpdateCollection> coll;
        if (FAILED(hr = CoCreateInstance(__uuidof(UpdateCollection), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&coll)))) { o.hr = hr; o.text = WuClassifyHr(hr).text; return o; }
        LONG idx = 0; coll->Add(up.Get(), &idx);
        Microsoft::WRL::ComPtr<IUpdateDownloader> dl;
        if (FAILED(hr = sess->CreateUpdateDownloader(&dl)) || FAILED(hr = dl->put_Updates(coll.Get()))) { o.hr = hr; o.text = WuClassifyHr(hr).text; return o; }
        dl->put_Priority(dpNormal);
        auto* cb = new WuNoopCallback;
        VARIANT state; VariantInit(&state);
        Microsoft::WRL::ComPtr<IDownloadJob> job;
        hr = dl->BeginDownload(static_cast<IDownloadProgressChangedCallback*>(cb), static_cast<IDownloadCompletedCallback*>(cb), state, &job);
        cb->Release();
        if (FAILED(hr) || !job) { o.hr = hr; o.text = WuClassifyHr(hr).text; return o; }
        bool abortSent = false;
        for (;;) {
            VARIANT_BOOL done = VARIANT_FALSE; job->get_IsCompleted(&done);
            Microsoft::WRL::ComPtr<IDownloadProgress> pr;
            if (SUCCEEDED(job->GetProgress(&pr)) && pr) { LONG pc = 0; if (SUCCEEDED(pr->get_PercentComplete(&pc))) progress(static_cast<int>(pc)); }
            if (done) break;
            if (!abortSent && cancel()) { job->RequestAbort(); abortSent = true; }   // "requested": Windows still has to stop; the loop goes on until the job says it is completed
            Sleep(250);
        }
        Microsoft::WRL::ComPtr<IDownloadResult> res;
        hr = dl->EndDownload(job.Get(), &res);
        OperationResultCode rc = orcNotStarted; LONG rh = 0;
        if (SUCCEEDED(hr) && res) { res->get_ResultCode(&rc); res->get_HResult(&rh); }
        job->CleanUp();
        if (abortSent || rc == orcAborted) { o.kind = WuDlKind::Aborted; o.hr = rh; return o; }
        if (FAILED(hr)) { o.hr = hr; o.text = WuClassifyHr(hr).text; return o; }
        if (rc == orcSucceeded || rc == orcSucceededWithErrors) {
            o.kind = WuDlKind::Done; o.hr = rh;
            Microsoft::WRL::ComPtr<IUpdate> again;   // read back from Windows: success is only what Windows says is downloaded now
            if (SUCCEEDED(Find(sess.Get(), u.id, false, again)) && again) { VARIANT_BOOL dld = VARIANT_FALSE; again->get_IsDownloaded(&dld); o.verifiedDownloaded = dld != VARIANT_FALSE; }
            return o;
        }
        o.kind = WuDlKind::Failed; o.hr = rh; o.text = WuClassifyHr(rh).text;
        return o;
    }

private:
    static HRESULT Find(IUpdateSession* sess, const std::wstring& id, bool online, Microsoft::WRL::ComPtr<IUpdate>& out) {
        Microsoft::WRL::ComPtr<IUpdateSearcher> se; HRESULT hr = sess->CreateUpdateSearcher(&se); if (FAILED(hr)) return hr;
        se->put_Online(online ? VARIANT_TRUE : VARIANT_FALSE);
        WuBstr crit(L"IsInstalled=0 and UpdateID='" + id + L"'");
        Microsoft::WRL::ComPtr<ISearchResult> sr; if (FAILED(hr = se->Search(crit.b, &sr))) return hr;
        Microsoft::WRL::ComPtr<IUpdateCollection> col; if (FAILED(hr = sr->get_Updates(&col))) return hr;
        LONG n = 0; col->get_Count(&n);
        if (n > 0) { Microsoft::WRL::ComPtr<IUpdate> first; if (SUCCEEDED(col->get_Item(0, &first))) out = first; }
        return S_OK;
    }
    static HRESULT Run(IUpdateSession* sess, const wchar_t* criteria, bool online, int limit, std::vector<WuUpdate>& out) {
        Microsoft::WRL::ComPtr<IUpdateSearcher> se; HRESULT hr = sess->CreateUpdateSearcher(&se); if (FAILED(hr)) return hr;
        se->put_Online(online ? VARIANT_TRUE : VARIANT_FALSE);
        WuBstr crit(criteria);
        Microsoft::WRL::ComPtr<ISearchResult> sr; if (FAILED(hr = se->Search(crit.b, &sr))) return hr;
        Microsoft::WRL::ComPtr<IUpdateCollection> col; if (FAILED(hr = sr->get_Updates(&col))) return hr;
        LONG n = 0; col->get_Count(&n);
        for (LONG i = 0; i < n && i < limit; ++i) {
            Microsoft::WRL::ComPtr<IUpdate> up; if (FAILED(col->get_Item(i, &up)) || !up) continue;
            out.push_back(Read(up.Get()));
        }
        return S_OK;
    }
    static WuUpdate Read(IUpdate* up) {
        WuUpdate w;
        BSTR b = nullptr;
        if (SUCCEEDED(up->get_Title(&b))) w.title = WuTake(b);
        b = nullptr; if (SUCCEEDED(up->get_Description(&b))) w.description = WuTake(b);
        Microsoft::WRL::ComPtr<IUpdateIdentity> ident;
        if (SUCCEEDED(up->get_Identity(&ident)) && ident) { b = nullptr; if (SUCCEEDED(ident->get_UpdateID(&b))) w.id = WuTake(b); LONG rev = 0; if (SUCCEEDED(ident->get_RevisionNumber(&rev))) w.revision = static_cast<int>(rev); }
        VARIANT_BOOL inst = VARIANT_FALSE, hid = VARIANT_FALSE, dld = VARIANT_FALSE;
        up->get_IsInstalled(&inst); up->get_IsHidden(&hid); up->get_IsDownloaded(&dld);
        w.state = WuStateFromFlags(inst != VARIANT_FALSE, hid != VARIANT_FALSE, dld != VARIANT_FALSE);
        DECIMAL dec{}; ULONGLONG sz = 0;
        if (SUCCEEDED(up->get_MaxDownloadSize(&dec)) && SUCCEEDED(VarUI8FromDec(&dec, &sz)) && sz > 0) { w.sizeBytes = sz; w.sizeKnown = true; }
        Microsoft::WRL::ComPtr<ICategoryCollection> cats;
        if (SUCCEEDED(up->get_Categories(&cats)) && cats) {
            LONG n = 0; cats->get_Count(&n);
            for (LONG i = 0; i < n && i < 2; ++i) { Microsoft::WRL::ComPtr<ICategory> c; BSTR nm = nullptr; if (SUCCEEDED(cats->get_Item(i, &c)) && c && SUCCEEDED(c->get_Name(&nm))) { if (!w.category.empty()) w.category += L", "; w.category += WuTake(nm); } }
        }
        std::vector<std::wstring> kbs;
        Microsoft::WRL::ComPtr<IStringCollection> sc;
        if (SUCCEEDED(up->get_KBArticleIDs(&sc)) && sc) { LONG n = 0; sc->get_Count(&n); for (LONG i = 0; i < n; ++i) { BSTR k = nullptr; if (SUCCEEDED(sc->get_Item(i, &k))) kbs.push_back(WuTake(k)); } }
        w.kb = WuKbFrom(kbs, w.title);
        return w;
    }
};

// Reads the Windows Update policy values (never writes them).
inline WuPolicy WuLivePolicy() {
    auto num = [](const wchar_t* sub, const wchar_t* value) -> long long {
        std::wstring key = L"SOFTWARE\\Policies\\Microsoft\\Windows\\WindowsUpdate"; if (sub && *sub) key += std::wstring(L"\\") + sub;
        DWORD v = 0, sz = sizeof(v);
        return RegGetValueW(HKEY_LOCAL_MACHINE, key.c_str(), value, RRF_RT_REG_DWORD, nullptr, &v, &sz) == ERROR_SUCCESS ? static_cast<long long>(v) : -1;
    };
    auto str = [](const wchar_t* value) -> std::wstring {
        wchar_t buf[512]; DWORD sz = sizeof(buf);
        return RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Policies\\Microsoft\\Windows\\WindowsUpdate", value, RRF_RT_REG_SZ, nullptr, buf, &sz) == ERROR_SUCCESS ? std::wstring(buf) : std::wstring();
    };
    return WuPolicyFrom(num, str);
}

}  // namespace ak
