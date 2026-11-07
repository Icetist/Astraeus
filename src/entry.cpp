#include "entry.h"

#include <bcrypt.h>
#include <objbase.h>
#include <shellapi.h>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "bcrypt.lib")

namespace {

constexpr ULONG kRounds = 300000; // slow on purpose: a guess costs real time
constexpr ULONG kSaltLen = 16, kKeyLen = 32, kNonceLen = 12, kTagLen = 16, kBoxLen = 96;
constexpr uint8_t kSaltA[16] = {0x86, 0x7D, 0x00, 0x2C, 0x8F, 0xED, 0x7D, 0x77, 0x3D, 0x56, 0xED, 0x84, 0x34, 0x3D, 0xA3, 0xFB};
constexpr uint8_t kSaltB[16] = {0x1C, 0x74, 0xCF, 0xA3, 0x29, 0x6E, 0x03, 0x78, 0xA9, 0x39, 0x4C, 0x59, 0x35, 0x69, 0x4C, 0x45};
constexpr uint8_t kCheck[32] = {0x5D, 0x93, 0xDE, 0x93, 0x89, 0xEE, 0x7F, 0x7F, 0x36, 0xF1, 0xA3, 0x8C, 0xA1, 0x07, 0x48, 0xA0, 0x88, 0xD3, 0xB7, 0x7D, 0x12, 0xB3, 0x42, 0xDF, 0x92, 0xA4, 0x76, 0xE2, 0xB7, 0x68, 0xD8, 0xB7};
constexpr uint8_t kNonce[12] = {0xC9, 0x16, 0xE6, 0x78, 0x97, 0x7B, 0x9E, 0xE5, 0x8E, 0x8A, 0x35, 0x6C};
constexpr uint8_t kTag[16] = {0x20, 0x0D, 0xE8, 0x58, 0xC4, 0x5A, 0xE5, 0x69, 0x12, 0x3F, 0xE7, 0xAF, 0x13, 0xDE, 0x73, 0x9B};
constexpr uint8_t kBox[kBoxLen] = {0xFF, 0x29, 0x49, 0x5F, 0x59, 0xC0, 0xD2, 0x43, 0x18, 0xB3, 0xAC, 0x3C, 0x70, 0x08, 0xAC, 0x2B, 0x9C, 0xD5, 0x55, 0x64, 0x67, 0xDA, 0x54, 0xB2, 0x48, 0x90, 0x73, 0x9B, 0xB9, 0x3E, 0x45, 0x4A, 0x08, 0x09, 0x5D, 0x75, 0x7C, 0x69, 0x08, 0x94, 0x36, 0xE7, 0xE2, 0xAB, 0xCE, 0x75, 0xAE, 0xB8, 0x88, 0x7B, 0x9E, 0x6C, 0xB1, 0xED, 0x5F, 0x2A, 0x11, 0xA6, 0xDF, 0xE9, 0xCB, 0xD1, 0x5C, 0x0B, 0xAE, 0x2A, 0xDD, 0x7A, 0x07, 0x08, 0xF4, 0xFF, 0x67, 0xCE, 0xCF, 0x6B, 0xD4, 0x13, 0x6E, 0x10, 0x69, 0xCD, 0xB8, 0x87, 0x9B, 0xF6, 0xF7, 0x95, 0xB2, 0xB6, 0x25, 0xC7, 0x45, 0x7F, 0x51, 0xFA};
constexpr uint8_t kSaltA2[16] = {0x2A, 0xEE, 0x7A, 0x85, 0x06, 0x04, 0x00, 0xB5, 0x75, 0x5E, 0x8F, 0xF2, 0x7C, 0x33, 0x79, 0x3B};
constexpr uint8_t kSaltB2[16] = {0xC8, 0x9F, 0x8B, 0x99, 0xA5, 0xED, 0xB5, 0x74, 0x61, 0xE7, 0x88, 0x46, 0x13, 0x73, 0x07, 0x50};
constexpr uint8_t kCheck2[32] = {0x3A, 0xFB, 0x90, 0x26, 0xB0, 0x56, 0x6E, 0x61, 0xFA, 0xB7, 0xA1, 0x54, 0x94, 0xF6, 0xA0, 0xCA, 0xC0, 0xD9, 0xC9, 0x0B, 0x16, 0x2B, 0x58, 0xF2, 0x96, 0x38, 0xC6, 0x61, 0xCD, 0x06, 0x76, 0x1A};
constexpr uint8_t kNonce2[12] = {0x26, 0x43, 0xC3, 0x13, 0xE0, 0x54, 0x24, 0x8E, 0x10, 0xDC, 0x4C, 0x59};
constexpr uint8_t kTag2[16] = {0x23, 0x77, 0x93, 0x67, 0xCE, 0xF0, 0x9C, 0x27, 0x1F, 0x84, 0x9A, 0x28, 0x2F, 0x67, 0xAD, 0xFD};
constexpr uint8_t kBox2[kBoxLen] = {0xD7, 0xA7, 0x68, 0xED, 0x2C, 0x1B, 0xA4, 0x08, 0x79, 0xAC, 0x57, 0x98, 0x24, 0x5E, 0xB3, 0xFE, 0x0B, 0x20, 0xFC, 0xDE, 0xD1, 0x98, 0xD0, 0x71, 0xF6, 0x2E, 0xDB, 0xD1, 0x9A, 0x0F, 0xE4, 0x97, 0x56, 0x0A, 0x74, 0xE9, 0x4B, 0x2E, 0x54, 0x74, 0x2A, 0x7A, 0xC9, 0xA4, 0x20, 0xDA, 0x7F, 0xC6, 0xD4, 0x9E, 0x6E, 0x34, 0x9A, 0x1A, 0x6F, 0xD7, 0x5A, 0x0D, 0x50, 0x5C, 0xA0, 0x92, 0x66, 0x47, 0xAF, 0x7E, 0x93, 0x78, 0x36, 0x6B, 0xA7, 0x95, 0x7C, 0x9A, 0x12, 0x13, 0x1D, 0x78, 0x19, 0x51, 0xEB, 0xAE, 0x10, 0x69, 0x3C, 0x18, 0x1D, 0x11, 0x0F, 0xD5, 0x6B, 0x3E, 0x9C, 0xD3, 0x62, 0x03};

struct Sealed { // one word's way in: its check, the salt its key comes from, and what it opens
    const uint8_t *saltA, *saltB, *check, *nonce, *tag, *box;
};
constexpr Sealed kSealed[] = {{kSaltA, kSaltB, kCheck, kNonce, kTag, kBox}, {kSaltA2, kSaltB2, kCheck2, kNonce2, kTag2, kBox2}};
constexpr size_t kSealedN = std::size(kSealed);

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

bool Open(const std::string& pw, std::string& out) { // every check at once, each on its own core; only the right text opens its box
    uint8_t keys[kSealedN][kKeyLen] = {}, checks[kSealedN][kKeyLen] = {};
    bool made[kSealedN][2] = {};
    std::vector<std::thread> work;
    for (size_t i = 0; i < kSealedN; ++i) {
        work.emplace_back([&, i] { made[i][0] = Derive(pw, kSealed[i].saltA, checks[i]); });
        work.emplace_back([&, i] { made[i][1] = Derive(pw, kSealed[i].saltB, keys[i]); });
    }
    for (std::thread& t : work) t.join();
    int hit = -1;
    for (size_t i = 0; i < kSealedN; ++i) // all of them looked at, whichever matches
        if (made[i][0] && made[i][1] && Same(checks[i], kSealed[i].check, kKeyLen) && hit < 0) hit = (int)i;
    SecureZeroMemory(checks, sizeof(checks));
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_KEY_HANDLE key = nullptr;
    bool ok = hit >= 0 && BCryptOpenAlgorithmProvider(&alg, BCRYPT_AES_ALGORITHM, nullptr, 0) == 0 &&
              BCryptSetProperty(alg, BCRYPT_CHAINING_MODE, (PUCHAR)BCRYPT_CHAIN_MODE_GCM, sizeof(BCRYPT_CHAIN_MODE_GCM), 0) == 0 &&
              BCryptGenerateSymmetricKey(alg, &key, nullptr, 0, keys[hit], kKeyLen, 0) == 0;
    SecureZeroMemory(keys, sizeof(keys));
    if (ok) {
        const Sealed& box = kSealed[hit];
        uint8_t tag[kTagLen];
        memcpy(tag, box.tag, kTagLen);
        BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
        BCRYPT_INIT_AUTH_MODE_INFO(info);
        info.pbNonce = (PUCHAR)box.nonce;
        info.cbNonce = kNonceLen;
        info.pbTag = tag;
        info.cbTag = kTagLen;
        out.assign(kBoxLen, '\0');
        ULONG got = 0;
        ok = BCryptDecrypt(key, (PUCHAR)box.box, kBoxLen, &info, nullptr, 0, (PUCHAR)out.data(), (ULONG)out.size(), &got, 0) == 0;
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
    if (s == 2) rightAt = now;
    job.reset();
    return false;
}
