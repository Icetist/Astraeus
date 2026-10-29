#pragma once
// The live page's sources, called on its worker only.
#include <atomic>
#include <string>
#include <vector>

#include "live.h"

namespace live::src {

struct Net { // one worker's request: cancelled as a whole when the app goes
    std::atomic<bool> stop{false};
    std::atomic<void*> slot{nullptr};
};

bool Get(Net& n, const std::wstring& url, std::string& body, size_t maxBytes = 4u << 20); // a few tries: dns here comes and goes
Card Fetch(Net& n, const Pick& p); // one picked sport, never throws; state kFailed when nothing came
Card World(Net& n); // the world's headlines
std::wstring Wide(const std::string& s);
std::string Utf8(const std::wstring& s);

struct Val { // a parsed json value
    enum Kind { kNull, kBool, kNum, kStr, kArr, kObj } kind = kNull;
    double num = 0;
    std::string str;
    std::vector<Val> items; // array elements, or object values
    std::vector<std::string> keys; // object keys, beside their values
    const Val& operator[](const char* key) const; // a null value when missing
    const Val& operator[](size_t i) const;
    const Val& operator[](int i) const { return (*this)[(size_t)i]; } // a literal 0 would read as a null key
    size_t size() const { return items.size(); }
    std::string Str() const; // numbers too, as text
    double Num() const;
    bool Truth() const { return kind == kBool ? num != 0 : kind == kStr ? str == "true" : false; }
    bool Null() const { return kind == kNull; }
};
bool Parse(const std::string& text, Val& out);
std::string Quote(const std::wstring& s); // a json string literal, for the cache

}
