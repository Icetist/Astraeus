#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace sys::detail {

bool Unbase64(const std::string& in, std::string& out); // standard alphabet, padding optional
bool Unzlib(const uint8_t* in, size_t n, std::string& out, size_t maxOut); // a zlib stream (2-byte header, deflate, checksum unchecked); false if malformed or too big

}
