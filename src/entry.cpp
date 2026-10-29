#include "entry.h"

#include <bcrypt.h>
#include <objbase.h>
#include <shellapi.h>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <string>
#include <thread>

#pragma comment(lib, "bcrypt.lib")

namespace {

constexpr ULONG kRounds = 300000; // slow on purpose: a guess costs real time
constexpr ULONG kSaltLen = 16, kKeyLen = 32, kNonceLen = 12, kTagLen = 16;
constexpr uint8_t kSaltA[16] = {0x86, 0x7D, 0x00, 0x2C, 0x8F, 0xED, 0x7D, 0x77, 0x3D, 0x56, 0xED, 0x84, 0x34, 0x3D, 0xA3, 0xFB};
constexpr uint8_t kSaltB[16] = {0x1C, 0x74, 0xCF, 0xA3, 0x29, 0x6E, 0x03, 0x78, 0xA9, 0x39, 0x4C, 0x59, 0x35, 0x69, 0x4C, 0x45};
constexpr uint8_t kCheck[32] = {0x5D, 0x93, 0xDE, 0x93, 0x89, 0xEE, 0x7F, 0x7F, 0x36, 0xF1, 0xA3, 0x8C, 0xA1, 0x07, 0x48, 0xA0, 0x88, 0xD3, 0xB7, 0x7D, 0x12, 0xB3, 0x42, 0xDF, 0x92, 0xA4, 0x76, 0xE2, 0xB7, 0x68, 0xD8, 0xB7};
constexpr uint8_t kNonce[12] = {0xC9, 0x16, 0xE6, 0x78, 0x97, 0x7B, 0x9E, 0xE5, 0x8E, 0x8A, 0x35, 0x6C};
constexpr uint8_t kTag[16] = {0x20, 0x0D, 0xE8, 0x58, 0xC4, 0x5A, 0xE5, 0x69, 0x12, 0x3F, 0xE7, 0xAF, 0x13, 0xDE, 0x73, 0x9B};
constexpr uint8_t kBox[96] = {0xFF, 0x29, 0x49, 0x5F, 0x59, 0xC0, 0xD2, 0x43, 0x18, 0xB3, 0xAC, 0x3C, 0x70, 0x08, 0xAC, 0x2B, 0x9C, 0xD5, 0x55, 0x64, 0x67, 0xDA, 0x54, 0xB2, 0x48, 0x90, 0x73, 0x9B, 0xB9, 0x3E, 0x45, 0x4A, 0x08, 0x09, 0x5D, 0x75, 0x7C, 0x69, 0x08, 0x94, 0x36, 0xE7, 0xE2, 0xAB, 0xCE, 0x75, 0xAE, 0xB8, 0x88, 0x7B, 0x9E, 0x6C, 0xB1, 0xED, 0x5F, 0x2A, 0x11, 0xA6, 0xDF, 0xE9, 0xCB, 0xD1, 0x5C, 0x0B, 0xAE, 0x2A, 0xDD, 0x7A, 0x07, 0x08, 0xF4, 0xFF, 0x67, 0xCE, 0xCF, 0x6B, 0xD4, 0x13, 0x6E, 0x10, 0x69, 0xCD, 0xB8, 0x87, 0x9B, 0xF6, 0xF7, 0x95, 0xB2, 0xB6, 0x25, 0xC7, 0x45, 0x7F, 0x51, 0xFA};

std::string Utf8(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)(std::max)(n, 0), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

bool Derive(const std::string& pw, const uint8_t* salt, uint8_t* out) {
    return BCryptDeriveKeyPBKDF2(BCRYPT_HMAC_SHA256_ALG_HANDLE, (PUCHAR)pw.data(), (ULONG)pw.size(), (PUCHAR)salt, kSaltLen, kRounds, out, kKeyLen, 0) == 0;
}

bool Same(const uint8_t* a, const uint8_t* b, size_t n) { // no early exit
    uint8_t d = 0;
    for (size_t i = 0; i < n; ++i) d |= a[i] ^ b[i];
    return d == 0;
}

bool Open(const std::string& pw, std::string& out) { // the check first, then the box: only the right text opens it
    uint8_t k[kKeyLen];
    bool ok = Derive(pw, kSaltA, k) && Same(k, kCheck, kKeyLen) && Derive(pw, kSaltB, k);
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_KEY_HANDLE key = nullptr;
    ok = ok && BCryptOpenAlgorithmProvider(&alg, BCRYPT_AES_ALGORITHM, nullptr, 0) == 0 &&
         BCryptSetProperty(alg, BCRYPT_CHAINING_MODE, (PUCHAR)BCRYPT_CHAIN_MODE_GCM, sizeof(BCRYPT_CHAIN_MODE_GCM), 0) == 0 &&
         BCryptGenerateSymmetricKey(alg, &key, nullptr, 0, k, kKeyLen, 0) == 0;
    SecureZeroMemory(k, sizeof(k));
    if (ok) {
        uint8_t tag[kTagLen];
        memcpy(tag, kTag, kTagLen);
        BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
        BCRYPT_INIT_AUTH_MODE_INFO(info);
        info.pbNonce = (PUCHAR)kNonce;
        info.cbNonce = kNonceLen;
        info.pbTag = tag;
        info.cbTag = kTagLen;
        out.assign(sizeof(kBox), '\0');
        ULONG got = 0;
        ok = BCryptDecrypt(key, (PUCHAR)kBox, sizeof(kBox), &info, nullptr, 0, (PUCHAR)out.data(), (ULONG)out.size(), &got, 0) == 0;
        if (!ok) SecureZeroMemory(out.data(), out.size()), out.clear();
    }
    if (key) BCryptDestroyKey(key);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

bool Handled(const std::wstring& target) { // something on this pc answers to the target's scheme
    size_t colon = target.find(L':');
    if (colon == std::wstring::npos || colon == 0) return false;
    HKEY h = nullptr;
    if (RegOpenKeyExW(HKEY_CLASSES_ROOT, target.substr(0, colon).c_str(), 0, KEY_READ, &h) != ERROR_SUCCESS) return false;
    bool yes = RegQueryValueExW(h, L"URL Protocol", nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
    RegCloseKey(h);
    return yes;
}

void Act(const std::string& what) { // one target per line: the first this pc can take
    int n = MultiByteToWideChar(CP_UTF8, 0, what.data(), (int)what.size(), nullptr, 0);
    std::wstring all((size_t)(std::max)(n, 0), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, what.data(), (int)what.size(), all.data(), n);
    std::wstring pick;
    for (size_t at = 0; at < all.size();) {
        size_t nl = all.find(L'\n', at);
        std::wstring line = all.substr(at, nl == std::wstring::npos ? std::wstring::npos : nl - at);
        at = nl == std::wstring::npos ? all.size() : nl + 1;
        if (pick.empty() || Handled(line)) pick = line;
        if (Handled(line)) break;
    }
    if (!pick.empty()) ShellExecuteW(nullptr, L"open", pick.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    SecureZeroMemory(all.data(), all.size() * sizeof(wchar_t));
}

}

Entry& Entry::Get() {
    static Entry e;
    return e;
}

void Entry::Type(wchar_t ch) {
    if (ch == 8) {
        if (!text.empty()) text.back() = 0, text.pop_back();
    } else if (ch >= 32 && ch != 127 && text.size() < kMax) {
        text += ch;
    }
}

void Entry::Drop() {
    focus = false;
    SecureZeroMemory(text.data(), text.size() * sizeof(wchar_t));
    text.clear();
}

bool Entry::Working() const { return job && job->state.load() == 0; }

void Entry::Submit() {
    size_t a = 0, b = text.size();
    while (a < b && iswspace(text[a])) ++a;
    while (b > a && iswspace(text[b - 1])) --b;
    std::string pw = Utf8(text.substr(a, b - a)); // exactly as typed, the ends trimmed
    Drop();
    if (pw.empty() || Working()) return;
    job = std::make_shared<Job>();
    std::thread([j = job, pw = std::move(pw)]() mutable {
        std::string out;
        bool ok = Open(pw, out);
        SecureZeroMemory(pw.data(), pw.size());
        if (ok) {
            HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE); // the shell wants it on this thread
            Act(out);
            SecureZeroMemory(out.data(), out.size());
            if (SUCCEEDED(co)) CoUninitialize();
        }
        j->state.store(ok ? 2 : 1);
    }).detach();
}

bool Entry::Settle(double now) {
    if (!job) return false;
    int s = job->state.load();
    if (s == 0) return true;
    if (s == 1) wrongAt = now;
    job.reset();
    return false;
}
