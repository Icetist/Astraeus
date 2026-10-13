#include "sys.h"

#include <oleauto.h>
#include <wbemidl.h>
#include <wrl/client.h>

#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <thread>

using Microsoft::WRL::ComPtr;

namespace sys {
namespace {

constexpr long kWmiMs = 5000;  // enum Next timeout, ms
constexpr int kSetSec = 1;  // WmiSetBrightness timeout arg, seconds
constexpr WPARAM kNone = static_cast<WPARAM>(-1);  // posted when level unknown

class Bstr {  // raii bstr, wmi wants BSTR not wchar_t*
public:
    explicit Bstr(const wchar_t* s) : b_(SysAllocString(s)) {}  // null on oom, check with bool
    Bstr(const Bstr&) = delete;
    Bstr& operator=(const Bstr&) = delete;
    ~Bstr() { SysFreeString(b_); }  // null safe
    BSTR get() const { return b_; }
    void reset(BSTR b) {  // takes ownership of b
        SysFreeString(b_);
        b_ = b;
    }
    explicit operator bool() const { return b_ != nullptr; }

private:
    BSTR b_;
};

class Variant {  // raii variant, clear frees owned bstr/objects
public:
    Variant() { VariantInit(&v); }  // sets VT_EMPTY
    Variant(const Variant&) = delete;
    Variant& operator=(const Variant&) = delete;
    ~Variant() { VariantClear(&v); }
    VARIANT v;
};

bool VarInt(const VARIANT& v, int& out) {  // wmi uint8 may come back as any int vt
    switch (v.vt) {
        case VT_UI1: out = v.bVal; return true;
        case VT_I2: out = v.iVal; return true;
        case VT_UI2: out = v.uiVal; return true;
        case VT_I4: out = v.lVal; return true;
        case VT_UI4: out = static_cast<int>(v.ulVal); return true;
        default: return false;
    }
}

}

struct Bright::Impl {
    std::mutex m;  // guards request fields below
    std::condition_variable cv;
    std::thread worker;  // wmi calls are slow, keep off ui thread
    bool stop = false;
    bool readReq = false;
    HWND readHwnd = nullptr;
    UINT readMsg = 0;
    std::optional<int> setReq;  // latest only, older sets coalesced

    ComPtr<IWbemServices> svc;  // worker thread only from here down
    bool connFail = false;
    bool noWmi = false;  // no wmi brightness (desktop monitor), stop trying
    Bstr methPath{nullptr};  // cached instance path for ExecMethod
    ComPtr<IWbemClassObject> inSig;  // cached in-params template

    ~Impl() {
        {
            std::lock_guard<std::mutex> lock(m);
            stop = true;
        }
        cv.notify_all();  // notify outside lock to avoid wake-then-block
        if (worker.joinable()) worker.join();
    }

    void Spawn() {  // lazy start, called under m
        if (worker.joinable()) return;
        worker = std::thread([this] { Run(); });
    }

    void Run() {
        const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);  // com is per thread; mta needs no pump
        for (;;) {
            bool doRead = false;
            HWND notify = nullptr;
            UINT msg = 0;
            std::optional<int> value;
            {
                std::unique_lock<std::mutex> lock(m);
                cv.wait(lock, [this] { return stop || readReq || setReq.has_value(); });  // predicate handles spurious wakes
                if (stop) break;
                doRead = readReq;  // copy requests out, then drop lock for wmi
                notify = readHwnd;
                msg = readMsg;
                readReq = false;
                value = setReq;
                setReq.reset();
            }
            try {
                if (value) WriteLvl(*value);  // write first so read reflects it
                if (doRead) {
                    const int pct = ReadLvl();
                    const WPARAM wp = pct < 0 ? kNone : static_cast<WPARAM>(pct);
                    if (notify && IsWindow(notify)) PostMessageW(notify, msg, wp, 0);  // result back to ui via wparam
                }
            } catch (...) {
                OutputDebugStringW(L"[sys] brightness worker error\n");
            }
        }
        inSig.Reset();  // release com objects before uninit
        svc.Reset();
        if (SUCCEEDED(co)) CoUninitialize();  // only balance a successful init
    }

    bool Connect() {
        if (svc) return true;
        if (connFail) return false;  // one attempt only
        connFail = true;  // cleared on success at end
        ComPtr<IWbemLocator> locator;
        if (FAILED(CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&locator)))) {
            OutputDebugStringW(L"[sys] WbemLocator unavailable\n");
            return false;
        }
        Bstr ns(L"ROOT\\WMI");  // monitor classes live here, not cimv2
        if (!ns || FAILED(locator->ConnectServer(ns.get(), nullptr, nullptr, nullptr, 0, nullptr, nullptr, &svc))) {
            OutputDebugStringW(L"[sys] WMI ConnectServer(root\\WMI) failed\n");
            svc.Reset();
            return false;
        }
        CoSetProxyBlanket(svc.Get(), RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr, RPC_C_AUTHN_LEVEL_CALL,  // impersonate or wmi calls get access denied
                          RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);
        connFail = false;
        return true;
    }

    ComPtr<IWbemClassObject> FirstObj(const wchar_t* wql) {
        ComPtr<IWbemClassObject> obj;
        Bstr lang(L"WQL");
        Bstr query(wql);
        ComPtr<IEnumWbemClassObject> en;
        if (!lang || !query ||
            FAILED(svc->ExecQuery(lang.get(), query.get(), WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,  // semisync, cheapest mode
                                  nullptr, &en))) {
            return obj;
        }
        ULONG got = 0;
        if (en->Next(kWmiMs, 1, &obj, &got) != WBEM_S_NO_ERROR || got == 0) obj.Reset();  // timeout returns S_FALSE, treat as none
        return obj;
    }

    int ReadLvl() {
        if (noWmi || !Connect()) return -1;
        ComPtr<IWbemClassObject> obj =
            FirstObj(L"SELECT CurrentBrightness FROM WmiMonitorBrightness WHERE Active=TRUE");  // only laptop panels expose this
        if (!obj) {
            noWmi = true;  // cache miss so we don't requery every tick
            OutputDebugStringW(L"[sys] WmiMonitorBrightness unsupported\n");
            return -1;
        }
        Variant v;
        int pct = -1;
        if (FAILED(obj->Get(L"CurrentBrightness", 0, &v.v, nullptr, nullptr)) || !VarInt(v.v, pct)) return -1;
        return std::clamp(pct, 0, 100);
    }

    bool PrepareSet() {
        if (methPath && inSig) return true;  // cached after first success
        ComPtr<IWbemClassObject> inst =
            FirstObj(L"SELECT * FROM WmiMonitorBrightnessMethods WHERE Active=TRUE");
        if (!inst) return false;
        Variant path;
        if (FAILED(inst->Get(L"__PATH", 0, &path.v, nullptr, nullptr)) || path.v.vt != VT_BSTR) return false;  // instance path needed to invoke method
        Bstr className(L"WmiMonitorBrightnessMethods");
        ComPtr<IWbemClassObject> cls;
        if (!className || FAILED(svc->GetObject(className.get(), 0, nullptr, &cls, nullptr))) return false;  // params sig comes from class, not instance
        ComPtr<IWbemClassObject> sig;
        if (FAILED(cls->GetMethod(L"WmiSetBrightness", 0, &sig, nullptr)) || !sig) return false;
        methPath.reset(SysAllocString(path.v.bstrVal));  // copy, variant frees its own
        if (!methPath) return false;
        inSig = sig;
        return true;
    }

    void WriteLvl(int percent) {
        if (noWmi || !Connect()) return;
        if (!PrepareSet()) {
            noWmi = true;
            OutputDebugStringW(L"[sys] WmiSetBrightness unsupported\n");
            return;
        }
        ComPtr<IWbemClassObject> args;
        if (FAILED(inSig->SpawnInstance(0, &args))) return;  // fresh in-params object per call
        Variant timeout;
        timeout.v.vt = VT_I4;  // uint32 in mof but wmi expects VT_I4
        timeout.v.lVal = kSetSec;
        Variant level;
        level.v.vt = VT_UI1;  // uint8 brightness
        level.v.bVal = static_cast<BYTE>(std::clamp(percent, 0, 100));
        Bstr method(L"WmiSetBrightness");
        if (!method || FAILED(args->Put(L"Timeout", 0, &timeout.v, 0)) ||
            FAILED(args->Put(L"Brightness", 0, &level.v, 0)) ||
            FAILED(svc->ExecMethod(methPath.get(), method.get(), 0, nullptr, args.Get(), nullptr, nullptr))) {
            OutputDebugStringW(L"[sys] WmiSetBrightness failed\n");
        }
    }
};

Bright::Bright() {
    try {
        impl = std::make_unique<Impl>();
    } catch (...) {
        OutputDebugStringW(L"[sys] Brightness init failed\n");
    }
}

Bright::~Bright() = default;

void Bright::ReadAsync(HWND notify, UINT msg) {
    try {
        if (!impl) {
            if (notify && IsWindow(notify)) PostMessageW(notify, msg, kNone, 0);  // still reply so ui stops waiting
            return;
        }
        {
            std::lock_guard<std::mutex> lock(impl->m);
            impl->readReq = true;  // repeated reads collapse into one
            impl->readHwnd = notify;
            impl->readMsg = msg;
            impl->Spawn();
        }
        impl->cv.notify_one();
    } catch (...) {
        OutputDebugStringW(L"[sys] Brightness::ReadAsync failed\n");
        if (notify && IsWindow(notify)) PostMessageW(notify, msg, kNone, 0);
    }
}

void Bright::Set(int percent) {
    try {
        if (!impl) return;
        {
            std::lock_guard<std::mutex> lock(impl->m);
            impl->setReq = std::clamp(percent, 0, 100);  // overwrite pending, slider drags spam this
            impl->Spawn();
        }
        impl->cv.notify_one();  // wake worker after lock released
    } catch (...) {
        OutputDebugStringW(L"[sys] Brightness::Set failed\n");
    }
}

}
