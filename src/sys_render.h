#pragma once
#include <atomic>
#include <string>

#include "clipedit.h"
#include "sys_audiofx.h"

// A clip made again from an edit: its picture decoded, retimed for speed and removed parts, framed tall or square, and
// encoded on the graphics card, with the finished sound laid under it. Slow: a worker thread only.
namespace sys::render {

bool Render(const std::wstring& src, const clipedit::Edit& e, const audiofx::Pcm& sound, const std::wstring& out, std::atomic<int>* percent, std::wstring& why);

}
