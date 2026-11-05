#include "sys.h"

#include <pdh.h>
#include <pdhmsg.h>

#include <algorithm>
#include <atomic>
#include <thread>

namespace sys {

std::atomic<int> gpuEvery{1}; // seconds between gpu reads; 0: nobody shows a gpu gauge, the counter is closed
void GpuWanted(int every) { gpuEvery = every; } // set by the model from what's on screen

namespace {

constexpr double kGb = 1024.0 * 1024.0 * 1024.0;  // bytes per gib
constexpr int kGpuRecycle = 240; // reads before the counter is closed and opened afresh: windows' gpu counter grows with every process it has seen
constexpr wchar_t kGpuPath[] = L"\\GPU Engine(*engtype_3D)\\Utilization Percentage";  // wildcard: one instance per process/engine

uint64_t FtU64(const FILETIME& ft) {
    return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;  // 100ns units
}

}

struct StatSrc::Impl {
    uint64_t prevIdle = 0, prevTotal = 0;
    bool haveCpu = false;
    PDH_HQUERY query = nullptr;
    PDH_HCOUNTER gpuCtr = nullptr;
    bool gpuOk = false;
    enum : int { kGpuIdle, kGpuPending, kGpuReady, kGpuFailed };
    std::atomic<int> gpuState{kGpuIdle}; // the counter is opened the first time a gauge wants it, never at launch
    std::thread gpuOpen; // opening it costs seconds of cpu: the gauge reads 0 until it's done
    std::vector<BYTE> gpuBuf;  // reused counter array buffer
    int threads = 0;

    Impl() {
        threads = static_cast<int>(GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));  // all groups, not just >64 cpu group 0
        ReadCpu();  // prime baseline so first sample has a delta
    }

    ~Impl() {
        if (gpuOpen.joinable()) gpuOpen.join();
        if (query) PdhCloseQuery(query);
    }

    void OpenGpu() {
        gpuState = kGpuPending;
        gpuOpen = std::thread([this] {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST); // a one-off, nobody waits on it
            InitGpu();
            gpuState.store(gpuOk ? kGpuReady : kGpuFailed, std::memory_order_release);
        });
    }

    void InitGpu() {
        PDH_STATUS st = PdhOpenQueryW(nullptr, 0, &query);
        if (st != ERROR_SUCCESS) {
            OutputDebugStringW(L"[sys] PdhOpenQuery failed; GPU unavailable\n");
            query = nullptr;
            return;
        }
        st = PdhAddEnglishCounterW(query, kGpuPath, 0, &gpuCtr);  // english name works on localized windows
        if (st != ERROR_SUCCESS) {
            OutputDebugStringW(L"[sys] GPU Engine counter unavailable\n");
            PdhCloseQuery(query);
            query = nullptr;
            return;
        }
        st = PdhCollectQueryData(query);  // rate counters need two collects
        if (st != ERROR_SUCCESS && st != PDH_NO_DATA) {  // no data is fine with zero instances
            OutputDebugStringW(L"[sys] GPU counter initial collect failed; GPU unavailable\n");
            PdhCloseQuery(query);
            query = nullptr;
            return;
        }
        gpuOk = true;
        gpuBuf.resize(16 * 1024);  // initial guess, grown on more data
    }

    double ReadCpu() {
        FILETIME idleFt, kernelFt, userFt;
        if (!GetSystemTimes(&idleFt, &kernelFt, &userFt)) return 0;
        const uint64_t idle = FtU64(idleFt);
        const uint64_t total = FtU64(kernelFt) + FtU64(userFt);  // kernel time already includes idle
        double cpu = 0;
        if (haveCpu && total > prevTotal) {  // need previous sample for a delta
            const uint64_t dTotal = total - prevTotal;
            const uint64_t dIdle = idle >= prevIdle ? idle - prevIdle : 0;  // guard against counter wrap
            cpu = dIdle >= dTotal ? 0.0 : static_cast<double>(dTotal - dIdle) / static_cast<double>(dTotal);  // busy = 1 - idle share
        }
        prevIdle = idle;
        prevTotal = total;
        haveCpu = true;
        return std::clamp(cpu, 0.0, 1.0);
    }

    double lastGpu = 0;
    int gpuTick = 0, gpuReads = 0;
    void CloseGpu() { // the query and everything windows built for it go; a gauge that comes back opens it afresh
        if (gpuState.load() == kGpuPending) return; // still opening on its thread
        if (gpuOpen.joinable()) gpuOpen.join();
        if (query) PdhCloseQuery(query);
        query = nullptr;
        gpuCtr = nullptr;
        gpuOk = false;
        gpuBuf.clear();
        gpuBuf.shrink_to_fit();
        gpuState = kGpuIdle;
        gpuReads = 0;
    }

    double ReadGpu() {
        int every = gpuEvery.load();
        if (every <= 0) { // no gauge shows it: nothing read, nothing held
            if (gpuState.load() == kGpuReady || gpuState.load() == kGpuFailed) CloseGpu();
            return lastGpu = 0;
        }
        if (gpuState.load() == kGpuReady && ++gpuTick % every != 0) return lastGpu; // a dashboard's gauge: a look every few seconds is plenty
        if (gpuState.load() == kGpuReady && ++gpuReads > kGpuRecycle) CloseGpu(); // its memory grows with time: start over now and then
        return lastGpu = ReadGpuNow();
    }

    double ReadGpuNow() {
        int state = gpuState.load(std::memory_order_acquire);
        if (state == kGpuIdle) OpenGpu();
        if (state == kGpuIdle || state == kGpuPending) return 0; // still opening: an empty gauge, not a missing one
        if (state == kGpuFailed) return -1;  // -1 tells ui gpu is unavailable
        PDH_STATUS st = PdhCollectQueryData(query);
        if (st != ERROR_SUCCESS) return 0;
        for (int attempt = 0; attempt < 3; ++attempt) {  // instance count can grow between calls
            DWORD bytes = static_cast<DWORD>(gpuBuf.size());  // in: capacity, out: required size
            DWORD count = 0;
            auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(gpuBuf.data());
            st = PdhGetFormattedCounterArrayW(gpuCtr, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &bytes,  // nocap: sum may exceed 100 per engine
                                              &count, items);
            if (st == PDH_MORE_DATA) {
                gpuBuf.resize(static_cast<size_t>(bytes) + 4096);  // grow with slack and retry
                continue;
            }
            if (st != ERROR_SUCCESS) return 0;
            double sum = 0;
            for (DWORD i = 0; i < count; ++i) {  // sum per-process 3d engine usage
                if (items[i].FmtValue.CStatus == PDH_CSTATUS_VALID_DATA ||  // skip stale/dead instances
                    items[i].FmtValue.CStatus == PDH_CSTATUS_NEW_DATA) {
                    sum += items[i].FmtValue.doubleValue;
                }
            }
            return std::clamp(sum / 100.0, 0.0, 1.0);  // percent to 0..1
        }
        return 0;  // buffer kept racing, give up this tick
    }

    void ReadMem(Stats& s) const {
        MEMORYSTATUSEX ms{};
        ms.dwLength = sizeof(ms);  // required or call fails
        if (!GlobalMemoryStatusEx(&ms) || ms.ullTotalPhys == 0) return;  // avoid div by zero
        const uint64_t used = ms.ullTotalPhys - ms.ullAvailPhys;  // avail includes standby cache
        s.memTotalGb = static_cast<double>(ms.ullTotalPhys) / kGb;
        s.memUsedGb = static_cast<double>(used) / kGb;
        s.mem = static_cast<double>(used) / static_cast<double>(ms.ullTotalPhys);
    }
};

StatSrc::StatSrc() {
    try {
        impl = std::make_unique<Impl>();
    } catch (...) {
        OutputDebugStringW(L"[sys] StatsSampler init failed\n");  // impl stays null, sample returns defaults
    }
}

StatSrc::~StatSrc() = default;

Stats StatSrc::Sample() {
    Stats s;
    try {
        if (!impl) return s;
        s.cpu = impl->ReadCpu();
        impl->ReadMem(s);
        s.gpu = impl->ReadGpu();
        s.threads = impl->threads;
    } catch (...) {
        OutputDebugStringW(L"[sys] StatsSampler::Sample failed\n");
    }
    return s;
}

}
