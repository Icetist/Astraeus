#include "sys.h"

#include <winternl.h>

#include <algorithm>
#include <string_view>

namespace sys {
namespace {

constexpr LONG kTooSmall = static_cast<LONG>(0xC0000004L);  // STATUS_INFO_LENGTH_MISMATCH
constexpr size_t kBufInit = 512 * 1024;  // fits typical process list first try
constexpr size_t kBufPad = 64 * 1024;  // slack, list grows between calls
constexpr int kTries = 5;

struct ProcEntry {  // head of SYSTEM_PROCESS_INFORMATION, real layout (winternl hides fields)
    ULONG NextEntryOffset;  // 0 on last entry
    ULONG NumberOfThreads;
    LARGE_INTEGER WorkingSetPrivateSize;  // private bytes, matches task manager memory
    ULONG HardFaultCount;
    ULONG NumberOfThreadsHighWatermark;
    ULONGLONG CycleTime;
    LARGE_INTEGER CreateTime;
    LARGE_INTEGER UserTime;  // 100ns units
    LARGE_INTEGER KernelTime;
    UNICODE_STRING ImageName;  // points into same buffer, not nul terminated
    LONG BasePriority;
    HANDLE UniqueProcessId;  // pid stored as handle-sized value
};

struct Prev {  // last cpu time per process for deltas
    DWORD pid;
    int64_t ctime;  // create time detects pid reuse
    uint64_t cpuT;
};

struct Entry {
    std::wstring_view name;  // view into buf, valid until next query
    uint64_t dCpu;
    uint64_t mem;
};

std::wstring_view StripExe(std::wstring_view n) {
    if (n.size() > 4) {  // keep name if it is only ".exe"
        const std::wstring_view ext = n.substr(n.size() - 4);
        if (_wcsnicmp(ext.data(), L".exe", 4) == 0) n.remove_suffix(4);
    }
    return n;
}

int CmpI(std::wstring_view a, std::wstring_view b) {  // case-insensitive, views aren't nul terminated
    const size_t n = std::min(a.size(), b.size());
    const int c = _wcsnicmp(a.data(), b.data(), n);
    if (c != 0) return c;
    return a.size() < b.size() ? -1 : (a.size() > b.size() ? 1 : 0);  // shorter prefix sorts first
}

bool IsSkip(DWORD pid, std::wstring_view name) {  // pid 0 = idle, kernel pseudo procs
    return pid == 0 || name.empty() || CmpI(name, L"System") == 0 ||
           CmpI(name, L"Idle") == 0;
}

}

struct ProcSrc::Impl {
    std::vector<BYTE> buf;  // reused across samples to avoid realloc
    std::vector<Prev> prev, cur;  // swapped each sample, sorted by pid
    std::vector<Entry> entries;
    LARGE_INTEGER prevQpc{};
    LARGE_INTEGER qpcHz{};
    bool havePrev = false;
    double cores = 1;  // cpu % is share of all cores

    Impl() {
        buf.resize(kBufInit);
        QueryPerformanceFrequency(&qpcHz);
        const DWORD n = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
        cores = n ? static_cast<double>(n) : 1.0;  // avoid div by zero
    }

    bool Query() {
        for (int attempt = 0; attempt < kTries; ++attempt) {
            ULONG needed = 0;
            const NTSTATUS st = NtQuerySystemInformation(SystemProcessInformation, buf.data(),  // one call, all processes
                                                         static_cast<ULONG>(buf.size()), &needed);
            if (st == kTooSmall) {
                buf.resize(std::max(buf.size() * 2, static_cast<size_t>(needed) + kBufPad));  // grow buffer and retry
                continue;
            }
            if (st < 0) {  // negative ntstatus = error
                OutputDebugStringW(L"[sys] NtQuerySystemInformation failed\n");
                return false;
            }
            return true;
        }
        return false;
    }

    uint64_t PrevCpu(DWORD pid, int64_t ctime, uint64_t now) const {
        auto it = std::lower_bound(prev.begin(), prev.end(), pid,  // binary search, prev sorted by pid
                                   [](const Prev& p, DWORD v) { return p.pid < v; });
        if (it == prev.end() || it->pid != pid || it->ctime != ctime) return now;  // new or reused pid: zero delta
        return it->cpuT <= now ? it->cpuT : now;  // never let delta go negative
    }

    void Collect(bool diff) {
        cur.clear();
        entries.clear();
        const BYTE* p = buf.data();
        for (;;) {  // walk linked list via byte offsets
            const auto* e = reinterpret_cast<const ProcEntry*>(p);
            const DWORD pid = static_cast<DWORD>(reinterpret_cast<ULONG_PTR>(e->UniqueProcessId));
            const uint64_t cpu = static_cast<uint64_t>(e->KernelTime.QuadPart) +
                                 static_cast<uint64_t>(e->UserTime.QuadPart);  // total cpu, 100ns
            const int64_t created = e->CreateTime.QuadPart;
            cur.push_back({pid, created, cpu});  // record even skipped ones
            const std::wstring_view name =
                e->ImageName.Buffer ? std::wstring_view(e->ImageName.Buffer, e->ImageName.Length / sizeof(wchar_t))  // Length is bytes
                                    : std::wstring_view();
            if (!IsSkip(pid, name)) {
                const uint64_t delta = diff ? cpu - PrevCpu(pid, created, cpu) : 0;  // first sample has no baseline
                const uint64_t mem = static_cast<uint64_t>(std::max<LONGLONG>(e->WorkingSetPrivateSize.QuadPart, 0));
                entries.push_back({StripExe(name), delta, mem});
            }
            if (e->NextEntryOffset == 0) break;
            p += e->NextEntryOffset;
        }
        std::sort(cur.begin(), cur.end(), [](const Prev& a, const Prev& b) { return a.pid < b.pid; });  // enables lower_bound next time
        prev.swap(cur);  // swap keeps both capacities
    }

    void Merge() {  // group same-name processes, e.g. chrome tabs
        std::sort(entries.begin(), entries.end(),
                  [](const Entry& a, const Entry& b) { return CmpI(a.name, b.name) < 0; });
        size_t out = 0;
        for (size_t i = 0; i < entries.size(); ++i) {  // in-place compaction
            const Entry& e = entries[i];
            if (out > 0 && CmpI(entries[out - 1].name, e.name) == 0) {
                entries[out - 1].dCpu += e.dCpu;
                entries[out - 1].mem += e.mem;
            } else {
                entries[out++] = e;
            }
        }
        entries.resize(out);
    }

    std::vector<ProcRow> Top(size_t n) {
        std::vector<ProcRow> result;
        if (n == 0 || !Query()) return result;
        LARGE_INTEGER qpc;
        QueryPerformanceCounter(&qpc);
        const bool diff = havePrev;
        double span = 0;
        if (diff && qpcHz.QuadPart > 0) {
            span = static_cast<double>(qpc.QuadPart - prevQpc.QuadPart) * 1e7 /  // elapsed in 100ns to match cpu times
                           static_cast<double>(qpcHz.QuadPart);
        }
        Collect(diff);
        prevQpc = qpc;
        havePrev = true;
        Merge();

        const size_t k = std::min(n, entries.size());
        std::partial_sort(entries.begin(), entries.begin() + static_cast<ptrdiff_t>(k), entries.end(),  // only need top k ordered
                          [](const Entry& a, const Entry& b) {
                              if (a.dCpu != b.dCpu) return a.dCpu > b.dCpu;
                              return a.mem > b.mem;  // tie-break by memory, idle procs stay stable
                          });
        result.reserve(k);
        const double denom = span * cores;  // max cpu time available across all cores
        for (size_t i = 0; i < k; ++i) {
            const Entry& e = entries[i];
            ProcRow info;
            info.name.assign(e.name);  // copy out, view dies with buf
            info.cpu = denom > 0 ? std::clamp(static_cast<double>(e.dCpu) / denom, 0.0, 1.0) : 0.0;
            info.memBytes = e.mem;
            result.push_back(std::move(info));
        }
        return result;
    }
};

ProcSrc::ProcSrc() {
    try {
        impl = std::make_unique<Impl>();
    } catch (...) {
        OutputDebugStringW(L"[sys] ProcSampler init failed\n");
    }
}

ProcSrc::~ProcSrc() = default;

std::vector<ProcRow> ProcSrc::Top(size_t n) {
    try {
        if (impl) return impl->Top(n);
    } catch (...) {
        OutputDebugStringW(L"[sys] ProcSampler::Top failed\n");
    }
    return {};
}

}
