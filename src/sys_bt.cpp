#include "bt.h"

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Bluetooth.GenericAttributeProfile.h>
#include <winrt/Windows.Devices.Bluetooth.Rfcomm.h>
#include <winrt/Windows.Devices.Enumeration.h>

#include <bluetoothapis.h>
#include <cfgmgr32.h>
#include <devicetopology.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <cwctype>
#include <map>
#include <mutex>
#include <vector>

using Microsoft::WRL::ComPtr;

#pragma comment(lib, "cfgmgr32.lib")
#include <devpropdef.h>

namespace sys {
namespace {

namespace WF = winrt::Windows::Foundation;
namespace DE = winrt::Windows::Devices::Enumeration;
namespace BT = winrt::Windows::Devices::Bluetooth;
namespace GA = winrt::Windows::Devices::Bluetooth::GenericAttributeProfile; // gatt, for le links
using Props = WF::Collections::IMapView<winrt::hstring, WF::IInspectable>; // aep property bag

constexpr wchar_t kClassicId[] = L"{e0cbf06c-cd8b-4647-bb8a-263b43f0f974}"; // aep protocol ids
constexpr wchar_t kLeId[] = L"{bb7bb05e-5972-42b5-94fc-76eaa7084d49}";
const GUID kLeGuid = {0xbb7bb05e, 0x5972, 0x42b5, {0x94, 0xfc, 0x76, 0xea, 0xa7, 0x08, 0x4d, 0x49}}; // same id as kLeId, binary form
constexpr wchar_t kProto[] = L"System.Devices.Aep.ProtocolId";
constexpr wchar_t kConn[] = L"System.Devices.Aep.IsConnected"; // live link state
constexpr wchar_t kAddr[] = L"System.Devices.Aep.DeviceAddress"; // aa:bb:cc:dd:ee:ff
constexpr wchar_t kMajor[] = L"System.Devices.Aep.Bluetooth.Cod.Major"; // classic device class
constexpr wchar_t kMinor[] = L"System.Devices.Aep.Bluetooth.Cod.Minor";
constexpr wchar_t kLeCat[] = L"System.Devices.Aep.Bluetooth.Le.Appearance.Category"; // le device class
constexpr uint64_t kOpMs = 15000; // give up showing "connecting" after 15s, room for one audio resend
constexpr auto kScanFor = std::chrono::seconds(40); // scanning drains power, auto stop
constexpr DWORD kCycleMs = 600; // gap between service off and on
constexpr auto kAudioPoll = std::chrono::milliseconds(250); // how often to check whether the link flipped
constexpr int kAudioResend = 20; // polls between driver requests, ~5s: one takes 3-4s to land, resending sooner restarts it
const GUID kBtAudioSet = {0x7fa06c40, 0xb8f6, 0x4c7e, {0x85, 0x56, 0xe8, 0xc3, 0x3a, 0x12, 0xe5, 0x4d}}; // KSPROPSETID_BtAudio
constexpr ULONG kKsGet = 1; // KSPROPERTY_TYPE_GET
constexpr DWORD kIoctlKsProperty = 0x2F0003; // CTL_CODE(FILE_DEVICE_KS, 0, METHOD_NEITHER, FILE_ANY_ACCESS)
const GUID kKsAudio = {0x6994ad04, 0x93ef, 0x11d0, {0xa3, 0xcc, 0x00, 0xa0, 0xc9, 0x22, 0x31, 0x96}}; // KSCATEGORY_AUDIO

struct KsProp { // KSPROPERTY layout, avoids pulling in ks.h
    GUID set;
    ULONG id; // 0 = one-shot reconnect, 1 = disconnect
    ULONG flags;
};

MIDL_INTERFACE("28F54685-06FD-11D2-B27A-00A0C9223196")
IKsCtl : public IUnknown { // IKsControl, only KsProperty used
public:
    virtual HRESULT STDMETHODCALLTYPE KsProperty(KsProp*, ULONG, void*, ULONG, ULONG*) = 0;
    virtual HRESULT STDMETHODCALLTYPE KsMethod(void*, ULONG, void*, ULONG, ULONG*) = 0;
    virtual HRESULT STDMETHODCALLTYPE KsEvent(void*, ULONG, void*, ULONG, ULONG*) = 0;
};

std::mutex gMu; // guards gLinks
std::map<std::wstring, GA::GattSession> gLinks; // le links we hold open, survive menu close

void Log(const wchar_t* what) {
    OutputDebugStringW((std::wstring(L"[bt] ") + what + L"\n").c_str()); // visible in debugview
}

std::wstring Lower(std::wstring s) {
    for (auto& c : s) c = static_cast<wchar_t>(std::towlower(c));
    return s;
}

WF::IPropertyValue Prop(const Props& m, const wchar_t* key) {
    if (!m.HasKey(key)) return nullptr; // Lookup throws on missing keys
    auto v = m.Lookup(key);
    return v ? v.try_as<WF::IPropertyValue>() : nullptr; // null if not a plain value
}

uint32_t Num(const WF::IPropertyValue& v) { // cod fields come back as various int widths
    if (!v) return 0;
    switch (v.Type()) {
    case WF::PropertyType::UInt8: return v.GetUInt8();
    case WF::PropertyType::UInt16: return v.GetUInt16();
    case WF::PropertyType::UInt32: return v.GetUInt32();
    case WF::PropertyType::Int32: return static_cast<uint32_t>(v.GetInt32());
    default: return 0;
    }
}

bool Flag(const WF::IPropertyValue& v) { return v && v.Type() == WF::PropertyType::Boolean && v.GetBoolean(); } // missing = false

std::wstring Str(const WF::IPropertyValue& v) {
    return v && v.Type() == WF::PropertyType::String ? std::wstring(v.GetString()) : std::wstring(); // missing = empty
}

bool Has(const std::wstring& s, std::initializer_list<const wchar_t*> keys) {
    for (const wchar_t* k : keys)
        if (s.find(k) != std::wstring::npos) return true;
    return false;
}

int KindOf(uint32_t major, uint32_t minor, uint32_t leCat, const std::wstring& name) {
    switch (major) { // classic class-of-device major
    case 1: return kBtPc;
    case 2: return kBtPhone;
    case 4: return kBtAudio; // audio/video
    case 5: { // peripheral: minor bits 5-4 keyboard/pointer, 3-0 subtype
        uint32_t lo = minor & 0xF, hi = (minor >> 4) & 3;
        if (lo == 1 || lo == 2) return kBtPad; // joystick, gamepad
        return hi == 2 ? kBtMouse : kBtKeys;
    }
    case 7: return kBtWatch;
    case 8: return kBtPad; // toy
    }
    switch (leCat) { // le appearance category
    case 1: return kBtPhone;
    case 2: return kBtPc;
    case 3: return kBtWatch;
    }
    std::wstring n = Lower(name); // last resort: guess from name
    if (Has(n, {L"bud", L"pod", L"head", L"ear", L"speaker", L"sound", L"audio", L"jbl", L"bose", L"beats", L"wh-", L"wf-"})) return kBtAudio;
    if (Has(n, {L"mouse", L"mx "})) return kBtMouse;
    if (Has(n, {L"controller", L"gamepad", L"xbox", L"dualsense", L"joy"})) return kBtPad;
    if (Has(n, {L"keyboard", L"keys"}) || leCat == 15) return kBtKeys; // 15 = hid
    if (Has(n, {L"watch", L"band"})) return kBtWatch;
    if (Has(n, {L"phone", L"galaxy", L"pixel"})) return kBtPhone;
    return kBtOther;
}

const DEVPROPKEY kBattery = {{0x104EA319, 0x6EE2, 0x4701, {0xBD, 0x47, 0x8D, 0xDB, 0xF4, 0x25, 0xBB, 0xE5}}, 2}; // bluetooth battery level, what settings shows
constexpr auto kChargeEvery = std::chrono::seconds(60); // batteries drain slowly: a look a minute while the menu is open

std::wstring MacIn(const std::wstring& id) { // the address inside a device instance id: the last run of exactly 12 hex digits
    std::wstring s = Lower(id), mac; // the last, because a service guid like {...-00805f9b34fb} comes first and is 12 digits too
    for (size_t i = 0; i < s.size();) {
        if (!std::iswxdigit(s[i])) {
            ++i;
            continue;
        }
        size_t j = i;
        while (j < s.size() && std::iswxdigit(s[j])) ++j;
        if (j - i == 12) mac = s.substr(i, 12);
        i = j;
    }
    return mac;
}

std::map<std::wstring, int> ReadCharges() { // battery by address, from the bluetooth device nodes that report one
    std::map<std::wstring, int> out;
    for (const wchar_t* bus : {L"BTHENUM", L"BTHLE"}) {
        ULONG len = 0;
        const ULONG flags = CM_GETIDLIST_FILTER_ENUMERATOR | CM_GETIDLIST_FILTER_PRESENT;
        if (CM_Get_Device_ID_List_SizeW(&len, bus, flags) != CR_SUCCESS || len < 2) continue;
        std::vector<wchar_t> ids(len);
        if (CM_Get_Device_ID_ListW(bus, ids.data(), len, flags) != CR_SUCCESS) continue;
        for (const wchar_t* id = ids.data(); *id; id += std::wcslen(id) + 1) {
            DEVINST node = 0;
            if (CM_Locate_DevNodeW(&node, const_cast<DEVINSTID_W>(id), CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS) continue;
            DEVPROPTYPE type = 0;
            BYTE pct = 0;
            ULONG size = sizeof(pct);
            if (CM_Get_DevNode_PropertyW(node, &kBattery, &type, &pct, &size, 0) != CR_SUCCESS || type != DEVPROP_TYPE_BYTE || pct > 100) continue;
            std::wstring mac = MacIn(id);
            if (!mac.empty()) out[mac] = pct;
        }
    }
    return out;
}

BtDev FromInfo(const DE::DeviceInformation& di, bool paired) {
    BtDev d;
    d.id = std::wstring(di.Id()); // aep id, used for every later call
    d.name = std::wstring(di.Name());
    d.paired = paired;
    Props p = di.Properties();
    d.on = Flag(Prop(p, kConn)); // unpaired finds report false
    for (wchar_t c : Str(Prop(p, kAddr))) // "aa:bb:.." -> "aabb.."
        if (std::iswxdigit(c)) d.mac += static_cast<wchar_t>(std::towlower(c));
    if (auto g = Prop(p, kProto); g && g.Type() == WF::PropertyType::Guid) {
        winrt::guid id = g.GetGuid();
        d.le = std::memcmp(&id, &kLeGuid, sizeof(GUID)) == 0; // decides connect path
    }
    d.kind = KindOf(Num(Prop(p, kMajor)), Num(Prop(p, kMinor)), Num(Prop(p, kLeCat)), d.name);
    d.audio = d.kind == kBtAudio; // try audio driver first
    return d;
}

DE::DeviceWatcher MakeWatcher(bool paired) {
    std::wstring aqs = std::wstring(L"(System.Devices.Aep.ProtocolId:=\"") + kClassicId + L"\" OR System.Devices.Aep.ProtocolId:=\"" + // classic or le, filtered by paired flag
                       kLeId + L"\") AND System.Devices.Aep.IsPaired:=System.StructuredQueryType.Boolean#" + (paired ? L"True" : L"False");
    try {
        return DE::DeviceInformation::CreateWatcher(
            aqs, winrt::single_threaded_vector<winrt::hstring>({winrt::hstring(kConn), winrt::hstring(kAddr), winrt::hstring(kProto),
                                                            winrt::hstring(kMajor), winrt::hstring(kMinor), winrt::hstring(kLeCat)}),
            DE::DeviceInformationKind::AssociationEndpoint);
    } catch (...) { // older builds reject unknown props, retry with core set
        return DE::DeviceInformation::CreateWatcher(aqs, winrt::single_threaded_vector<winrt::hstring>({winrt::hstring(kConn), winrt::hstring(kAddr), winrt::hstring(kProto)}),
                                                    DE::DeviceInformationKind::AssociationEndpoint);
    }
}

void StopWatch(const DE::DeviceWatcher& w) {
    if (!w) return;
    try {
        auto st = w.Status(); // stop throws unless running
        if (st == DE::DeviceWatcherStatus::Started || st == DE::DeviceWatcherStatus::EnumerationCompleted) w.Stop();
    } catch (...) {
        Log(L"watcher stop failed");
    }
}

bool KsToggle(const std::wstring& mac, bool on) { // audio: ask bt audio driver to (re)connect, like settings does
    if (mac.size() != 12) return false;
    ComPtr<IMMDeviceEnumerator> en; // core audio device list
    ComPtr<IMMDeviceCollection> all;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en))) ||
        FAILED(en->EnumAudioEndpoints(eAll, DEVICE_STATEMASK_ALL, &all))) // all states, disconnected ones too
        return false;
    UINT n = 0;
    all->GetCount(&n); // every endpoint incl. unplugged
    std::vector<std::wstring> done; // a2dp + hfp endpoints share one filter
    bool hit = false;
    for (UINT i = 0; i < n; ++i) {
        ComPtr<IMMDevice> dev;
        ComPtr<IDeviceTopology> topo, back;
        ComPtr<IConnector> pin, peer;
        ComPtr<IPart> part;
        if (FAILED(all->Item(i, &dev)) ||
            FAILED(dev->Activate(__uuidof(IDeviceTopology), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(topo.GetAddressOf()))) ||
            FAILED(topo->GetConnector(0, &pin)) || FAILED(pin->GetConnectedTo(&peer)) || FAILED(peer.As(&part)) ||
            FAILED(part->GetTopologyObject(&back))) // endpoint -> ks filter behind it
            continue;
        LPWSTR raw = nullptr; // filter device id
        if (FAILED(back->GetDeviceId(&raw))) continue;
        std::wstring fid(raw); // keep original case for GetDevice
        CoTaskMemFree(raw);
        if (Lower(fid).find(mac) == std::wstring::npos) continue; // filter path embeds the bt address
        if (std::find(done.begin(), done.end(), fid) != done.end()) continue;
        done.push_back(fid);
        ComPtr<IMMDevice> filt;
        ComPtr<IKsCtl> ks;
        if (FAILED(en->GetDevice(fid.c_str(), &filt)) ||
            FAILED(filt->Activate(__uuidof(IKsCtl), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(ks.GetAddressOf()))))
            continue;
        KsProp p = {kBtAudioSet, on ? 0ul : 1ul, kKsGet}; // a get request triggers the action
        ULONG got = 0;
        if (SUCCEEDED(ks->KsProperty(&p, sizeof(p), nullptr, 0, &got))) hit = true;
    }
    return hit;
}

bool KsDirect(const std::wstring& mac, bool on) { // newer stacks hide the topology link, talk to the filter itself
    if (mac.size() != 12) return false;
    GUID cat = kKsAudio;
    ULONG len = 0;
    if (CM_Get_Device_Interface_List_SizeW(&len, &cat, nullptr, CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS || len < 2)
        return false;
    std::vector<wchar_t> list(len);
    if (CM_Get_Device_Interface_ListW(&cat, nullptr, list.data(), len, CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS)
        return false;
    bool hit = false;
    for (const wchar_t* p = list.data(); *p; p += wcslen(p) + 1) { // double-null terminated list
        if (Lower(p).find(mac) == std::wstring::npos) continue; // bt filter paths embed the address
        HANDLE h = CreateFileW(p, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;
        KsProp prop = {kBtAudioSet, on ? 0ul : 1ul, kKsGet};
        DWORD got = 0;
        if (DeviceIoControl(h, kIoctlKsProperty, &prop, sizeof(prop), nullptr, 0, &got, nullptr)) hit = true; // same request IKsControl sends
        CloseHandle(h);
    }
    return hit;
}

bool CycleServices(const std::wstring& mac) { // classic: services off then on drops the link, keeps pairing
    if (mac.size() != 12) return false;
    BLUETOOTH_DEVICE_INFO info = {sizeof(info)};
    info.Address.ullLong = wcstoull(mac.c_str(), nullptr, 16); // 12 hex digits -> 48-bit address
    BLUETOOTH_FIND_RADIO_PARAMS rp = {sizeof(rp)};
    HANDLE radio = nullptr;
    HBLUETOOTH_RADIO_FIND f = BluetoothFindFirstRadio(&rp, &radio); // first local adapter
    if (!f) return false;
    BluetoothFindRadioClose(f); // radio handle stays valid
    bool ok = false;
    GUID svc[32];
    DWORD n = 32; // in: capacity, out: count
    if (BluetoothGetDeviceInfo(radio, &info) == ERROR_SUCCESS &&
        BluetoothEnumerateInstalledServices(radio, &info, &n, svc) == ERROR_SUCCESS) {
        for (DWORD i = 0; i < n; ++i) BluetoothSetServiceState(radio, &info, &svc[i], BLUETOOTH_SERVICE_DISABLE);
        ::Sleep(kCycleMs); // let the stack tear down before re-enabling
        for (DWORD i = 0; i < n; ++i)
            ok = BluetoothSetServiceState(radio, &info, &svc[i], BLUETOOTH_SERVICE_ENABLE) == ERROR_SUCCESS || ok;
    }
    CloseHandle(radio); // from FindFirstRadio
    return ok;
}

WF::IAsyncOperation<bool> PageClassic(std::wstring id) { // sdp query pages the device, link comes up
    auto dev = co_await BT::BluetoothDevice::FromIdAsync(id);
    if (!dev) co_return false; // device gone or id stale
    co_await dev.GetRfcommServicesAsync(BT::BluetoothCacheMode::Uncached); // uncached forces radio traffic
    dev.Close();
    co_return true;
}

WF::IAsyncOperation<bool> HoldLe(std::wstring id) { // le: a maintained gatt session keeps it connected
    auto s = co_await GA::GattSession::FromDeviceIdAsync(BT::BluetoothDeviceId::FromId(id));
    if (!s) co_return false;
    s.MaintainConnection(true); // os keeps link up while we hold s
    std::lock_guard lock(gMu);
    gLinks.insert_or_assign(id, s); // hold until disconnect or exit
    co_return true;
}

bool DropLe(const std::wstring& id) {
    GA::GattSession s{nullptr};
    {
        std::lock_guard lock(gMu);
        auto it = gLinks.find(id);
        if (it == gLinks.end()) return false; // os owns the link, can't force it
        s = it->second;
        gLinks.erase(it);
    }
    s.Close(); // drops our hold, link may idle out
    return true;
}

}

struct Bt::Impl : std::enable_shared_from_this<Bt::Impl> {
    mutable std::mutex mu;
    std::vector<BtDev> devs; // paired + discovered, unsorted
    std::atomic<HWND> hwnd{nullptr};
    UINT msg = 0; // posted to hwnd on any change
    DE::DeviceWatcher known{nullptr}, disc{nullptr}; // paired / discovery
    std::atomic<bool> scanning{false};
    std::atomic<int> gen{0}; // bumps on every scan change, stale auto-stops skip
    CO_MTA_USAGE_COOKIE mta = nullptr; // released in dtor
    std::map<std::wstring, int> charge; // battery by address, refreshed while open

    Impl() { CoIncrementMTAUsage(&mta); } // keep an mta so pool threads can use com
    ~Impl() {
        if (mta) CoDecrementMTAUsage(mta);
    }

    void Ping() const {
        HWND h = hwnd.load();
        if (h && IsWindow(h)) PostMessageW(h, msg, 0, 0); // ui re-snaps on this
    }

    BtDev* Find(const std::wstring& id) {
        for (auto& d : devs)
            if (d.id == id) return &d;
        return nullptr;
    }

    void Upsert(BtDev d) {
        {
            std::lock_guard lock(mu);
            if (BtDev* o = Find(d.id)) { // keep pending op state across refresh
                d.want = o->want;
                d.until = o->until;
                d.pairing = o->pairing;
                *o = std::move(d);
            } else {
                devs.push_back(std::move(d)); // new device
            }
        }
        Ping();
    }

    void Patch(const DE::DeviceInformationUpdate& u) {
        {
            std::lock_guard lock(mu);
            BtDev* d = Find(std::wstring(u.Id()));
            if (!d) return;
            Props p = u.Properties();
            if (p.HasKey(kConn)) d->on = Flag(Prop(p, kConn)); // connect/disconnect events land here
        }
        Ping();
    }

    void Drop(const std::wstring& id, bool paired) {
        {
            std::lock_guard lock(mu); // only drop from the list that reported it
            devs.erase(std::remove_if(devs.begin(), devs.end(),
                                      [&](const BtDev& d) { return d.id == id && d.paired == paired && !d.pairing; }),
                       devs.end());
        }
        Ping();
    }

    DE::DeviceWatcher Watch(bool paired) {
        DE::DeviceWatcher w = MakeWatcher(paired);
        std::weak_ptr<Impl> ws = weak_from_this(); // handlers must not keep us alive
        w.Added([ws, paired](const DE::DeviceWatcher&, const DE::DeviceInformation& di) {
            auto s = ws.lock();
            if (!s) return;
            try {
                BtDev d = FromInfo(di, paired);
                if (!d.name.empty()) s->Upsert(std::move(d)); // nameless le beacons are noise
            } catch (...) {
                Log(L"add failed");
            }
        });
        w.Updated([ws](const DE::DeviceWatcher&, const DE::DeviceInformationUpdate& u) { // property changes
            if (auto s = ws.lock()) try { s->Patch(u); } catch (...) {}
        });
        w.Removed([ws, paired](const DE::DeviceWatcher&, const DE::DeviceInformationUpdate& u) { // unpaired or out of range
            if (auto s = ws.lock()) try { s->Drop(std::wstring(u.Id()), paired); } catch (...) {}
        });
        w.Start(); // all three handlers must exist before start
        return w;
    }

    void Scan(bool on) {
        DE::DeviceWatcher old{nullptr};
        if (on) {
            if (scanning.exchange(true)) return; // already scanning
            DE::DeviceWatcher w{nullptr};
            try {
                w = Watch(false);
            } catch (...) {
                scanning.store(false);
                Log(L"scan start failed");
                return;
            }
            {
                std::lock_guard lock(mu);
                disc = w;
            }
            AutoStop(++gen); // arm the 40s cutoff
        } else {
            if (!scanning.exchange(false)) return; // not scanning
            ++gen;
            {
                std::lock_guard lock(mu);
                old = disc;
                disc = nullptr;
                devs.erase(std::remove_if(devs.begin(), devs.end(), [](const BtDev& d) { return !d.paired && !d.pairing; }),
                           devs.end()); // forget unpaired finds
            }
            StopWatch(old);
        }
        Ping();
    }

    winrt::fire_and_forget Charges() { // battery levels while the menu is open; stops with it
        auto self = shared_from_this();
        try {
            for (;;) {
                co_await winrt::resume_background();
                if (!hwnd.load()) co_return;
                auto got = ReadCharges();
                bool changed;
                {
                    std::lock_guard lock(mu);
                    changed = got != charge;
                    charge = std::move(got);
                }
                if (changed) Ping();
                co_await winrt::resume_after(kChargeEvery);
                if (!hwnd.load()) co_return;
            }
        } catch (...) {
            Log(L"battery read failed");
        }
    }

    winrt::fire_and_forget AutoStop(int g) {
        auto self = shared_from_this();
        co_await winrt::resume_after(kScanFor); // no thread blocked while waiting
        if (gen.load() == g) Scan(false); // user may have restarted scan meanwhile
    }

    void Shutdown() {
        DE::DeviceWatcher a{nullptr}, b{nullptr}; // stopped outside the lock
        hwnd.store(nullptr); // in-flight jobs stop posting
        {
            std::lock_guard lock(mu);
            a = known;
            b = disc;
            known = disc = nullptr;
            devs.clear();
            devs.shrink_to_fit();
        }
        scanning.store(false);
        ++gen;
        StopWatch(a); // watchers release their handlers
        StopWatch(b);
    }

    enum class Op { Waiting, Done, Dropped };

    Op OpState(const std::wstring& id, bool on) {
        std::lock_guard lock(mu);
        BtDev* x = Find(id);
        if (!x || x->want != on || x->until <= GetTickCount64()) return Op::Dropped; // gone, superseded or timed out
        return x->on == on ? Op::Done : Op::Waiting;
    }

    WF::IAsyncOperation<bool> AudioLink(BtDev d, bool on) { // driver is missing right after pairing and ignores requests while the link settles: resend until it flips
        auto self = shared_from_this();
        bool sent = false;
        for (int tick = 0;; ++tick) {
            if (tick % kAudioResend == 0 && (KsToggle(d.mac, on) || KsDirect(d.mac, on))) sent = true;
            co_await winrt::resume_after(kAudioPoll);
            Op st = OpState(d.id, on);
            if (st == Op::Done) co_return true;
            if (st == Op::Dropped) co_return sent; // deadline or newer op; unsent lets Link try other paths
        }
    }

    winrt::fire_and_forget Link(BtDev d, bool on) {
        auto self = shared_from_this();
        co_await winrt::resume_background(); // driver calls block, keep off ui thread
        bool ok = false;
        try {
            if (d.audio) ok = co_await AudioLink(d, on); // headphones, speakers
            if (!ok && d.le) ok = on ? co_await HoldLe(d.id) : DropLe(d.id);
            else if (!ok && on) ok = co_await PageClassic(d.id);
            else if (!ok && !d.audio) ok = CycleServices(d.mac); // never on audio: it drops sound with no way back
        } catch (...) {
            Log(L"connect failed");
        }
        if (!ok) { // nothing happened, stop the spinner now
            std::lock_guard lock(mu);
            if (BtDev* x = Find(d.id); x && x->want == on) x->until = 0; // a newer op owns the spinner otherwise
        }
        Ping();
    }

    winrt::fire_and_forget PairJob(std::wstring id) {
        auto self = shared_from_this();
        co_await winrt::resume_background();
        try {
            auto di = co_await DE::DeviceInformation::CreateFromIdAsync(id, winrt::single_threaded_vector<winrt::hstring>(),
                                                                        DE::DeviceInformationKind::AssociationEndpoint);
            auto custom = di.Pairing().Custom(); // custom pairing lets us answer prompts
            auto rev = custom.PairingRequested(winrt::auto_revoke, [](const DE::DeviceInformationCustomPairing&,
                                                                      const DE::DevicePairingRequestedEventArgs& a) {
                if (a.PairingKind() == DE::DevicePairingKinds::ProvidePin) a.Accept(L"0000"); // legacy default pin
                else a.Accept(); // just-works / numeric compare
            });
            auto kinds = DE::DevicePairingKinds::ConfirmOnly | DE::DevicePairingKinds::ConfirmPinMatch |
                         DE::DevicePairingKinds::DisplayPin | DE::DevicePairingKinds::ProvidePin;
            auto r = co_await custom.PairAsync(kinds); // paired watcher picks it up on success
            if (r.Status() != DE::DevicePairingResultStatus::Paired && r.Status() != DE::DevicePairingResultStatus::AlreadyPaired)
                Log(L"pair rejected");
        } catch (...) {
            Log(L"pair failed");
        }
        {
            std::lock_guard lock(mu);
            if (BtDev* x = Find(id)) x->pairing = false;
        }
        Ping();
    }

    winrt::fire_and_forget ForgetJob(std::wstring id) {
        auto self = shared_from_this();
        co_await winrt::resume_background();
        try {
            DropLe(id); // release our le hold first
            auto di = co_await DE::DeviceInformation::CreateFromIdAsync(id, winrt::single_threaded_vector<winrt::hstring>(),
                                                                        DE::DeviceInformationKind::AssociationEndpoint);
            co_await di.Pairing().UnpairAsync(); // watcher removes the row
        } catch (...) {
            Log(L"forget failed");
        }
        Ping();
    }
};

Bt::Bt() = default;

Bt::~Bt() {
    Close();
    std::map<std::wstring, GA::GattSession> links;
    {
        std::lock_guard lock(gMu);
        links.swap(gLinks); // release le sessions before com shuts down
    }
    for (auto& [id, s] : links) try { s.Close(); } catch (...) {} // close outside the lock
}

void Bt::Open(HWND hwnd, UINT msg) {
    if (impl) return;
    try {
        auto s = std::make_shared<Impl>();
        s->hwnd.store(hwnd);
        s->msg = msg;
        s->known = s->Watch(true); // paired list starts filling now
        s->Charges();
        impl = std::move(s);
    } catch (...) {
        Log(L"open failed");
    }
}

void Bt::Close() {
    auto s = std::move(impl); // freed once in-flight jobs finish
    if (s) try { s->Shutdown(); } catch (...) {} // watchers stop, list cleared
}

void Bt::Scan(bool on) {
    if (impl) try { impl->Scan(on); } catch (...) {}
}

bool Bt::Scanning() const { return impl && impl->scanning.load(); }

void Bt::Snap(std::vector<BtDev>& out) const {
    out.clear();
    if (!impl) return;
    {
        std::lock_guard lock(impl->mu);
        out = impl->devs; // copy under lock, sort outside
        for (auto& d : out) {
            auto it = impl->charge.find(d.mac);
            d.battery = it != impl->charge.end() ? it->second : -1;
        }
    }
    std::sort(out.begin(), out.end(), [](const BtDev& a, const BtDev& b) { // paired, then connected, then name
        if (a.paired != b.paired) return a.paired;
        if (a.on != b.on) return a.on;
        return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0;
    });
}

void Bt::Connect(const std::wstring& id, bool on) {
    if (!impl) return;
    BtDev d;
    {
        std::lock_guard lock(impl->mu);
        BtDev* x = impl->Find(id);
        if (!x) return;
        x->want = on;
        x->until = GetTickCount64() + kOpMs; // ui shows progress until state flips or deadline
        d = *x;
    }
    impl->Ping();
    try { impl->Link(d, on); } catch (...) {} // runs on a pool thread
}

void Bt::Pair(const std::wstring& id) {
    if (!impl) return;
    {
        std::lock_guard lock(impl->mu);
        BtDev* x = impl->Find(id);
        if (!x || x->pairing) return; // ignore double clicks
        x->pairing = true;
    }
    impl->Ping();
    try { impl->PairJob(id); } catch (...) {}
}

void Bt::Forget(const std::wstring& id) {
    if (impl) try { impl->ForgetJob(id); } catch (...) {}
}

}
