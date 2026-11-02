// The shelf on disk: one dpapi-encrypted file written atomically, images it keeps encrypted beside it, and the previews the worker makes.
#include "shelf_impl.h"

#include <dpapi.h>
#include <objbase.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cwchar>
#include <unordered_set>

#include "sys.h"

#pragma comment(lib, "crypt32.lib")

using Microsoft::WRL::ComPtr;

namespace sys {

namespace {

constexpr wchar_t kKeepVal[] = L"ShelfKeep";
constexpr wchar_t kDatName[] = L"\\shelf.dat", kOldName[] = L"\\shelf.txt"; // the old plain file is read once, then gone
constexpr wchar_t kImgExt[] = L".img";
constexpr char kSalt[] = "Astraeus shelf"; // entropy: another app running as you can't just ask dpapi for it
constexpr wchar_t kHeader[] = L"astraeus-shelf 2";
constexpr size_t kSaveTextMax = 16 * 1024; // a text is written this long at most
constexpr size_t kFileMax = 64u << 20, kOldMax = 1u << 20;
constexpr UINT kDibMaxPx = 8192; // a bigger picture pastes as its file only
constexpr int64_t kUnixDiff = 11644473600LL; // seconds from 1601 to 1970

DATA_BLOB Salt() { return {(DWORD)(sizeof(kSalt) - 1), (BYTE*)kSalt}; }

std::wstring Escape(const std::wstring& s) { // one field per tab, one record per line
    std::wstring o;
    o.reserve(s.size());
    for (wchar_t c : s) {
        if (c == L'\\') o += L"\\\\";
        else if (c == L'\n') o += L"\\n";
        else if (c == L'\r') o += L"\\r";
        else if (c == L'\t') o += L"\\t";
        else o += c;
    }
    return o;
}

std::wstring Unescape(const std::wstring& s) {
    std::wstring o;
    o.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != L'\\' || i + 1 == s.size()) {
            o += s[i];
            continue;
        }
        wchar_t e = s[++i];
        o += e == L'n' ? L'\n' : e == L'r' ? L'\r' : e == L't' ? L'\t' : e;
    }
    return o;
}

std::vector<std::wstring> Fields(const std::wstring& line) {
    std::vector<std::wstring> f;
    for (size_t at = 0;;) {
        size_t end = line.find(L'\t', at);
        f.push_back(line.substr(at, end == std::wstring::npos ? std::wstring::npos : end - at));
        if (end == std::wstring::npos) return f;
        at = end + 1;
    }
}

template <class Fn>
void Lines(const std::wstring& all, Fn fn) {
    for (size_t at = 0; at < all.size();) {
        size_t end = all.find(L'\n', at);
        if (end == std::wstring::npos) end = all.size();
        fn(all.substr(at, end - at));
        at = end + 1;
    }
}

ComPtr<IWICImagingFactory> Wic() { // per call: the worker and the ui thread each make their own
    ComPtr<IWICImagingFactory> f;
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f));
    return f;
}

ComPtr<IWICBitmapFrameDecode> Frame(IWICImagingFactory* wic, const std::string& bytes, const std::wstring& file, ComPtr<IWICStream>& keep) {
    ComPtr<IWICBitmapDecoder> dec;
    if (!bytes.empty()) {
        if (FAILED(wic->CreateStream(&keep)) || FAILED(keep->InitializeFromMemory((BYTE*)bytes.data(), (DWORD)bytes.size()))) return nullptr;
        if (FAILED(wic->CreateDecoderFromStream(keep.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &dec))) return nullptr;
    } else if (FAILED(wic->CreateDecoderFromFilename(file.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec))) {
        return nullptr;
    }
    ComPtr<IWICBitmapFrameDecode> fr;
    if (FAILED(dec->GetFrame(0, &fr))) return nullptr;
    return fr;
}

bool Pixels32(IWICImagingFactory* wic, IWICBitmapSource* src, UINT maxPx, const GUID& fmt, UINT& w, UINT& h, std::vector<uint32_t>& px, bool bottomUp) {
    UINT sw = 0, sh = 0;
    if (FAILED(src->GetSize(&sw, &sh)) || !sw || !sh) return false;
    double k = (std::min)(1.0, (double)maxPx / (std::max)(sw, sh)); // never upscaled
    w = (std::max)(1u, (UINT)(sw * k));
    h = (std::max)(1u, (UINT)(sh * k));
    ComPtr<IWICBitmapSource> cur = src;
    ComPtr<IWICBitmapScaler> sc;
    if ((w != sw || h != sh) && SUCCEEDED(wic->CreateBitmapScaler(&sc)) && SUCCEEDED(sc->Initialize(src, w, h, WICBitmapInterpolationModeFant))) cur = sc;
    ComPtr<IWICFormatConverter> cv;
    if (FAILED(wic->CreateFormatConverter(&cv)) || FAILED(cv->Initialize(cur.Get(), fmt, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) return false;
    px.assign((size_t)w * h, 0);
    if (!bottomUp) return SUCCEEDED(cv->CopyPixels(nullptr, w * 4, (UINT)(px.size() * 4), (BYTE*)px.data()));
    for (UINT y = 0; y < h; ++y) { // a dib's rows run bottom to top
        WICRect row = {0, (INT)y, (INT)w, 1};
        if (FAILED(cv->CopyPixels(&row, w * 4, w * 4, (BYTE*)&px[(size_t)(h - 1 - y) * w]))) return false;
    }
    return true;
}

bool DibToPng(const std::string& dib, std::string& png) { // a clipboard bitmap made into a png
    if (dib.size() < sizeof(BITMAPINFOHEADER)) return false;
    const auto* bi = reinterpret_cast<const BITMAPINFOHEADER*>(dib.data());
    if (bi->biSize < sizeof(BITMAPINFOHEADER) || bi->biSize > dib.size()) return false;
    DWORD colors = bi->biClrUsed ? bi->biClrUsed : bi->biBitCount <= 8 ? 1u << bi->biBitCount : 0;
    DWORD masks = bi->biCompression == BI_BITFIELDS && bi->biSize == sizeof(BITMAPINFOHEADER) ? 12 : 0;
    BITMAPFILEHEADER fh{};
    fh.bfType = 0x4D42; // "BM": the bmp decoder reads every header kind and row order for us
    fh.bfSize = (DWORD)(sizeof(fh) + dib.size());
    fh.bfOffBits = (DWORD)(sizeof(fh) + bi->biSize + masks + colors * 4);
    std::string bmp((const char*)&fh, sizeof(fh));
    bmp += dib;
    ComPtr<IWICImagingFactory> wic = Wic();
    ComPtr<IWICStream> in;
    ComPtr<IWICBitmapFrameDecode> fr = wic ? Frame(wic.Get(), bmp, L"", in) : nullptr;
    if (!fr) return false;
    ComPtr<IStream> out;
    ComPtr<IWICBitmapEncoder> enc;
    ComPtr<IWICBitmapFrameEncode> fe;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &out)) || FAILED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) ||
        FAILED(enc->Initialize(out.Get(), WICBitmapEncoderNoCache)) || FAILED(enc->CreateNewFrame(&fe, nullptr)) || FAILED(fe->Initialize(nullptr)) ||
        FAILED(fe->WriteSource(fr.Get(), nullptr)) || FAILED(fe->Commit()) || FAILED(enc->Commit()))
        return false;
    HGLOBAL mem = nullptr;
    if (FAILED(GetHGlobalFromStream(out.Get(), &mem))) return false;
    STATSTG st{};
    out->Stat(&st, STATFLAG_NONAME);
    if (const void* p = GlobalLock(mem)) {
        png.assign((const char*)p, (size_t)st.cbSize.QuadPart);
        GlobalUnlock(mem);
    }
    return !png.empty();
}

bool ShellThumb(const std::wstring& path, UINT px, Shelf::Pixels& out) { // explorer's thumbnail, or the type's icon
    ComPtr<IShellItemImageFactory> f;
    if (FAILED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&f)))) return false; // moved or deleted since
    HBITMAP bmp = nullptr;
    DWORD attr = GetFileAttributesW(path.c_str());
    bool remote = PathIsNetworkPathW(path.c_str()) || (attr != INVALID_FILE_ATTRIBUTES && (attr & (FILE_ATTRIBUTE_OFFLINE | FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS)));
    SIIGBF flags = (SIIGBF)(SIIGBF_RESIZETOFIT | (remote ? SIIGBF_INCACHEONLY : 0)); // a share or a cloud placeholder: only what's cached, never a download
    if (FAILED(f->GetImage({(LONG)px, (LONG)px}, flags, &bmp)) || !bmp) return false;
    BITMAP bm{};
    GetObjectW(bmp, sizeof(bm), &bm);
    bool ok = bm.bmWidth > 0 && bm.bmHeight > 0;
    if (ok) {
        out.w = (UINT)bm.bmWidth;
        out.h = (UINT)bm.bmHeight;
        out.px.assign((size_t)out.w * out.h, 0);
        BITMAPINFO bi{};
        bi.bmiHeader = {sizeof(BITMAPINFOHEADER), bm.bmWidth, -bm.bmHeight, 1, 32, BI_RGB}; // top-down
        HDC dc = GetDC(nullptr);
        ok = GetDIBits(dc, bmp, 0, out.h, out.px.data(), &bi, DIB_RGB_COLORS) == (int)out.h;
        ReleaseDC(nullptr, dc);
    }
    DeleteObject(bmp);
    if (!ok) return false;
    bool alpha = std::any_of(out.px.begin(), out.px.end(), [](uint32_t p) { return (p >> 24) != 0; });
    for (uint32_t& p : out.px) {
        if (!alpha) { // photos come back with no alpha at all: opaque
            p |= 0xFF000000u;
            continue;
        }
        uint32_t a = p >> 24, r = (std::min)((p >> 16) & 0xFF, a), g = (std::min)((p >> 8) & 0xFF, a), b = (std::min)(p & 0xFF, a); // a valid premultiplied pixel
        p = a << 24 | r << 16 | g << 8 | b;
    }
    out.icon = alpha && !shelfkind::IsImageName(path); // a document's or a folder's icon: it sits on frosted glass
    return true;
}

void SetAside(const std::wstring& file) { // unreadable: kept for whoever wants it, never overwritten
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t stamp[48];
    swprintf_s(stamp, L".unreadable-%04u%02u%02u-%02u%02u%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    MoveFileExW(file.c_str(), (file + stamp).c_str(), MOVEFILE_WRITE_THROUGH);
    OutputDebugStringW(L"[shelf] saved shelf unreadable, set aside\n");
}

void DeleteAll(const std::wstring& dir, const wchar_t* pattern, const std::unordered_set<std::wstring>* keepSet = nullptr) {
    if (dir.empty()) return;
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW((dir + L"\\" + pattern).c_str(), &fd);
    if (f == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::wstring p = dir + L"\\" + fd.cFileName;
        if (!keepSet || !keepSet->count(p)) DeleteFileW(p.c_str());
    } while (FindNextFileW(f, &fd));
    FindClose(f);
}

void WipeTemp() { // owned images made real for a drag or a paste, from the last run
    std::wstring t = shelfio::TempDir();
    if (t.empty()) return;
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW((t + L"\\*").c_str(), &fd);
    if (f == INVALID_HANDLE_VALUE) return;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
        std::wstring sub = t + L"\\" + fd.cFileName;
        DeleteAll(sub, L"*");
        RemoveDirectoryW(sub.c_str());
    } while (FindNextFileW(f, &fd));
    FindClose(f);
}

}

namespace shelfio {

std::wstring Dir() {
    PWSTR base = nullptr;
    std::wstring path;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &base))) path = std::wstring(base) + L"\\Astraeus";
    CoTaskMemFree(base);
    return path;
}

std::wstring ImageDir() {
    std::wstring d = Dir();
    return d.empty() ? d : d + L"\\shelf";
}

std::wstring TempDir() {
    wchar_t buf[MAX_PATH + 1];
    DWORD n = GetTempPathW(MAX_PATH + 1, buf);
    return n ? std::wstring(buf, n) + L"Astraeus Shelf" : L"";
}

std::string Utf8(const std::wstring& s) {
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    std::string out(n > 0 ? n : 0, '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring Wide(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring out(n > 0 ? n : 0, L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), n);
    return out;
}

bool Protect(const std::string& plain, std::string& blob) {
    DATA_BLOB in = {(DWORD)plain.size(), (BYTE*)plain.data()}, salt = Salt(), out{};
    if (!CryptProtectData(&in, L"Astraeus shelf", &salt, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return false; // only this windows user can read it back
    blob.assign((const char*)out.pbData, out.cbData);
    LocalFree(out.pbData);
    return true;
}

bool Unprotect(const std::string& blob, std::string& plain) {
    DATA_BLOB in = {(DWORD)blob.size(), (BYTE*)blob.data()}, salt = Salt(), out{};
    if (blob.empty() || !CryptUnprotectData(&in, nullptr, &salt, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return false; // another user's, or damaged
    plain.assign((const char*)out.pbData, out.cbData);
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return true;
}

bool ReadAll(const std::wstring& path, std::string& out, size_t max) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(f, &size) && size.QuadPart > 0 && (uint64_t)size.QuadPart <= max;
    if (ok) {
        out.resize((size_t)size.QuadPart);
        DWORD got = 0;
        ok = ReadFile(f, out.data(), (DWORD)out.size(), &got, nullptr) && got == out.size();
    }
    CloseHandle(f);
    return ok;
}

bool WriteAtomic(const std::wstring& path, const std::string& bytes) {
    SHCreateDirectoryExW(nullptr, path.substr(0, path.find_last_of(L'\\')).c_str(), nullptr);
    std::wstring tmp = path + L".tmp";
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    bool ok = WriteFile(f, bytes.data(), (DWORD)bytes.size(), &written, nullptr) && written == bytes.size();
    CloseHandle(f);
    if (ok && MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
    DeleteFileW(tmp.c_str()); // a torn write never replaces the last good one
    return false;
}

int64_t Now() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return (int64_t)(((uint64_t)ft.dwHighDateTime << 32 | ft.dwLowDateTime) / 10000000ULL) - kUnixDiff;
}

uint64_t Hash(const void* data, size_t n) { // fnv-1a 64
    uint64_t h = 1469598103934665603ULL;
    const auto* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 1099511628211ULL;
    return h;
}

bool StoreImage(Shelf::Work::Img& img, ShelfFile& out) {
    std::string png;
    if (img.dib) {
        if (!DibToPng(img.bytes, png)) return false;
        img.bytes.swap(png);
    }
    if (img.bytes.empty() || img.bytes.size() > kImageMax) return false;
    std::string blob;
    if (!Protect(img.bytes, blob)) return false;
    out.hash = Hash(img.bytes.data(), img.bytes.size());
    wchar_t leaf[48];
    swprintf_s(leaf, L"\\%016llx%08x%s", (unsigned long long)out.hash, (unsigned)GetTickCount(), kImgExt);
    out.path = ImageDir() + leaf;
    out.name = img.name;
    out.owned = true;
    SecureZeroMemory(img.bytes.data(), img.bytes.size());
    return WriteAtomic(out.path, blob);
}

bool LoadImage(const std::wstring& path, std::string& bytes) {
    std::string blob;
    return ReadAll(path, blob, kImageMax * 2) && Unprotect(blob, bytes);
}

bool Preview(const std::wstring& path, bool owned, UINT px, Shelf::Pixels& out) {
    if (!owned) return ShellThumb(path, px, out);
    std::string bytes;
    ComPtr<IWICImagingFactory> wic = Wic();
    ComPtr<IWICStream> keepStream;
    if (!wic || !LoadImage(path, bytes)) return false;
    ComPtr<IWICBitmapFrameDecode> fr = Frame(wic.Get(), bytes, L"", keepStream);
    bool ok = fr && Pixels32(wic.Get(), fr.Get(), px, GUID_WICPixelFormat32bppPBGRA, out.w, out.h, out.px, false);
    SecureZeroMemory(bytes.data(), bytes.size());
    return ok;
}

bool Dib(const std::string& encoded, const std::wstring& file, std::string& dib) {
    ComPtr<IWICImagingFactory> wic = Wic();
    ComPtr<IWICStream> keepStream;
    ComPtr<IWICBitmapFrameDecode> fr = wic ? Frame(wic.Get(), encoded, file, keepStream) : nullptr;
    UINT w = 0, h = 0;
    std::vector<uint32_t> px;
    if (!fr || !Pixels32(wic.Get(), fr.Get(), kDibMaxPx, GUID_WICPixelFormat32bppBGRA, w, h, px, true)) return false;
    BITMAPINFOHEADER bi = {sizeof(BITMAPINFOHEADER), (LONG)w, (LONG)h, 1, 32, BI_RGB}; // bottom-up: what most apps expect from a paste
    bi.biSizeImage = w * h * 4;
    dib.assign((const char*)&bi, sizeof(bi));
    dib.append((const char*)px.data(), px.size() * 4);
    return true;
}

std::wstring Serialize(const std::vector<ShelfItem>& items, size_t textMax) {
    std::wstring body = std::wstring(kHeader) + L"\n";
    wchar_t num[64];
    for (const auto& it : items) {
        swprintf_s(num, L"%d\t%s\t%d\t%lld\t%06X\t", (int)it.kind, it.pinned ? L"p" : L"-", it.count, (long long)it.at, it.rgb & 0xFFFFFF);
        body += L"I\t" + std::wstring(num) + Escape(it.note) + L"\t" + Escape(it.text.substr(0, textMax)) + L"\n"; // a long text keeps its start
        for (const auto& f : it.files) {
            swprintf_s(num, L"%s%s\t%016llX\t", f.dir ? L"d" : L"", f.owned ? L"o" : f.dir ? L"" : L"-", (unsigned long long)f.hash);
            body += L"F\t" + std::wstring(num) + Escape(f.name) + L"\t" + Escape(f.path) + L"\n";
        }
    }
    return body;
}

bool Parse(const std::wstring& body, std::vector<ShelfItem>& out) {
    if (body.compare(0, wcslen(kHeader), kHeader) != 0) return false;
    bool first = true;
    Lines(body, [&](const std::wstring& line) {
        if (first) {
            first = false;
            return;
        }
        std::vector<std::wstring> f = Fields(line);
        if (f[0] == L"I" && f.size() >= 8) {
            ShelfItem it;
            int kind = _wtoi(f[1].c_str());
            it.kind = kind >= 0 && kind < (int)ShelfKind::kCount ? (ShelfKind)kind : ShelfKind::Text;
            it.pinned = f[2] == L"p";
            it.count = (std::max)(1, _wtoi(f[3].c_str()));
            it.at = _wtoi64(f[4].c_str());
            it.rgb = (uint32_t)wcstoul(f[5].c_str(), nullptr, 16) & 0xFFFFFF;
            it.note = Unescape(f[6]).substr(0, Shelf::kNoteMax);
            it.text = Unescape(f[7]);
            out.push_back(std::move(it));
        } else if (f[0] == L"F" && f.size() >= 5 && !out.empty() && out.back().files.size() < Shelf::kStackMax) {
            ShelfFile sf;
            sf.dir = f[1].find(L'd') != std::wstring::npos;
            sf.owned = f[1].find(L'o') != std::wstring::npos;
            sf.hash = wcstoull(f[2].c_str(), nullptr, 16);
            sf.name = Unescape(f[3]);
            sf.path = Unescape(f[4]);
            if (!sf.path.empty()) out.back().files.push_back(std::move(sf));
        }
    });
    return true;
}

void ParseOld(const std::wstring& body, std::vector<ShelfItem>& out) {
    Lines(body, [&](const std::wstring& line) {
        if (line.size() < 3 || line[1] != L'\t') return;
        ShelfItem it;
        if (line[0] == L'F') {
            ShelfFile sf;
            sf.path = line.substr(2);
            sf.name = PathFindFileNameW(sf.path.c_str());
            it.files.push_back(std::move(sf));
        } else if (line[0] == L'T') {
            it.text = Unescape(line.substr(2));
        } else {
            return;
        }
        out.push_back(std::move(it)); // the file is newest first already
    });
}

}

void Shelf::Save() {
    saveDue = false;
    std::wstring dir = shelfio::Dir();
    if (!keep || dir.empty()) return; // off: nothing of the shelf reaches the disk but the images it holds, encrypted, wiped at the next launch
    std::string body = shelfio::Utf8(shelfio::Serialize(items, kSaveTextMax)), blob;
    bool ok = shelfio::Protect(body, blob) && shelfio::WriteAtomic(dir + kDatName, blob);
    SecureZeroMemory(body.data(), body.size());
    if (!ok) OutputDebugStringW(L"[shelf] save failed\n");
}

void Shelf::Load() {
    std::wstring dir = shelfio::Dir();
    if (dir.empty()) return;
    std::string blob, plain;
    std::vector<ShelfItem> got;
    bool migrated = false;
    if (shelfio::ReadAll(dir + kDatName, blob, kFileMax)) {
        if (!shelfio::Unprotect(blob, plain) || !shelfio::Parse(shelfio::Wide(plain), got)) {
            SetAside(dir + kDatName); // its images stay too: they may be what someone wants back
            return;
        }
        SecureZeroMemory(plain.data(), plain.size());
    } else if (shelfio::ReadAll(dir + kOldName, blob, kOldMax)) {
        shelfio::ParseOld(shelfio::Wide(blob), got); // the plain shelf of before, moved over once
        migrated = true;
    }
    std::unordered_set<std::wstring> owned;
    int64_t now = shelfio::Now();
    for (auto& it : got) {
        std::vector<ShelfFile> files;
        for (auto& f : it.files) {
            DWORD a = GetFileAttributesW(f.path.c_str());
            if (a == INVALID_FILE_ATTRIBUTES) {
                std::wstring folder = f.path.substr(0, f.path.find_last_of(L'\\'));
                if (f.owned || GetFileAttributesW(folder.c_str()) != INVALID_FILE_ATTRIBUTES) continue; // deleted since: dropped quietly
                a = f.dir ? FILE_ATTRIBUTE_DIRECTORY : 0; // its drive or share isn't there yet at sign-in: kept, or the next save loses it for good
            }
            f.dir = !f.owned && (a & FILE_ATTRIBUTE_DIRECTORY);
            if (f.name.empty()) f.name = PathFindFileNameW(f.path.c_str());
            files.push_back(std::move(f));
        }
        bool hadFiles = !it.files.empty();
        it.files = std::move(files);
        if (hadFiles && it.files.empty()) continue;
        if (migrated) { // the old file knew no kinds or times
            it.at = now;
            if (it.HasFiles()) it.kind = !it.files[0].dir && shelfkind::IsImageName(it.files[0].path) ? ShelfKind::Image : ShelfKind::File;
            else it.kind = shelfkind::Classify(it.text, it.rgb);
        }
        if (opts.clearStart && !it.Kept()) continue; // its owned images go with the sweep below
        it.id = nextId++;
        for (const auto& f : it.files)
            if (f.owned) owned.insert(f.path);
        items.push_back(std::move(it));
    }
    DeleteAll(shelfio::ImageDir(), L"*.img", &owned); // images no card holds any more
    if (migrated || opts.clearStart) Save();
    if (migrated) DeleteFileW((dir + kOldName).c_str()); // its plain text is now only in the encrypted file
}

void Shelf::SetKeep(bool on) {
    keep = on;
    SaveNum(kKeepVal, on ? 1 : 0);
    if (on) {
        Save();
        return;
    }
    std::wstring dir = shelfio::Dir();
    if (dir.empty()) return;
    DeleteFileW((dir + kDatName).c_str()); // nothing of the shelf stays on disk
    DeleteFileW((dir + kOldName).c_str());
}

void ShelfWipe() {
    std::wstring dir = shelfio::Dir();
    if (dir.empty()) return;
    DeleteFileW((dir + kDatName).c_str());
    DeleteFileW((dir + kDatName + std::wstring(L".tmp")).c_str());
    DeleteFileW((dir + kOldName).c_str());
    DeleteAll(shelfio::ImageDir(), L"*");
    RemoveDirectoryW(shelfio::ImageDir().c_str());
    WipeTemp();
}

namespace shelfio {
void Sweep(bool images) { // at launch: last run's temp copies, and while not keeping, every image it held
    WipeTemp();
    if (images) DeleteAll(ImageDir(), L"*.img");
}
bool Kept() { return ReadNum(kKeepVal, 0) != 0; }
}

}
