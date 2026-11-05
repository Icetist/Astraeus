#pragma once
// a problem report the user copies and shares themselves: nothing here touches the network
#include <string>
#include <vector>

namespace sys {

std::wstring Scrub(std::wstring text, std::vector<std::wstring> names); // each name, whole words in any case, becomes <private>; the longest go first so a path goes before the folder in it
std::wstring LogPick(const std::wstring& log); // the lines worth reading, in order: what went wrong outlasts any number of plain starts
std::wstring ProblemReport(); // version, this pc, a short list of plain settings, the end of the log; scrubbed
bool CopyText(const std::wstring& text); // false: another app is holding the clipboard

}
