#include <library/cpp/getopt/last_getopt.h>

#include <util/generic/string.h>
#include <util/generic/vector.h>
#include <util/generic/ymath.h>
#include <util/stream/output.h>
#include <util/string/split.h>
#include <util/string/strip.h>
#include <util/system/compiler.h>
#include <util/system/datetime.h>
#include <util/system/hp_timer.h>
#include <util/system/types.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>

#ifdef _linux_
#include <pthread.h>
#include <sched.h>
#endif

namespace {

    struct TConfig {
        ui64 SizeBytes = 4096;
        ui64 WorkingSetBytes = 256ULL << 20;
        ui32 Threads = 1;
        double WarmupSec = 1.0;
        double DurationSec = 5.0;
        ui64 Seed = 1;
        TString CpuList;
        bool SharedSeqCstAtomicBeforeMemcpy = false;
    };

    struct TThreadResult {
        ui64 Calls = 0;
        ui64 PayloadBytes = 0;
        ui64 TrafficBytes = 0;
        ui64 Cycles = 0;
        double Seconds = 0.0;
        ui64 Sink = 0;
        i32 Cpu = -1;
    };

    ui64 NextRand(ui64& state) {
        state += 0x9e3779b97f4a7c15ULL;
        ui64 z = state;
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
        return z ^ (z >> 31);
    }

    void FillRandom(TVector<ui8>& data, ui64 seed) {
        ui64 state = seed;
        for (ui64 i = 0; i < data.size(); ++i) {
            if ((i & 7ULL) == 0) {
                state = NextRand(state);
            }
            data[i] = static_cast<ui8>(state >> ((i & 7ULL) * 8ULL));
        }
    }

    TVector<i32> ParseCpuList(const TString& cpuList) {
        TVector<i32> cpus;
        if (cpuList.empty()) {
            return cpus;
        }

        for (const TStringBuf part : StringSplitter(cpuList).Split(',')) {
            TStringBuf trimmed = StripString(part);
            if (trimmed.empty()) {
                continue;
            }
            const auto dash = trimmed.find('-');
            if (dash == TStringBuf::npos) {
                cpus.push_back(FromString<i32>(trimmed));
                continue;
            }

            const i32 begin = FromString<i32>(trimmed.Head(dash));
            const i32 end = FromString<i32>(trimmed.Skip(dash + 1));
            Y_ABORT_UNLESS(begin <= end, "Invalid CPU range: %s", TString(trimmed).c_str());
            for (i32 cpu = begin; cpu <= end; ++cpu) {
                cpus.push_back(cpu);
            }
        }

        return cpus;
    }

#ifdef _linux_
    void PinThread(i32 cpu) {
        if (cpu < 0) {
            return;
        }

        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(cpu, &cpuset);
        Y_ABORT_UNLESS(
            pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset) == 0,
            "pthread_setaffinity_np(%d) failed",
            cpu);
    }

    i32 CurrentCpu() {
        return sched_getcpu();
    }
#else
    void PinThread(i32) {
    }

    i32 CurrentCpu() {
        return -1;
    }
#endif

    Y_FORCE_INLINE void DoOps(
        ui8* memsetDst,
        ui8* memcpyDst,
        const ui8* memcpySrc,
        size_t size,
        ui8 memsetValue,
        std::atomic<ui64>* sharedCounter)
    {
        std::memset(memsetDst, memsetValue, size);
        if (sharedCounter) {
            sharedCounter->fetch_add(1, std::memory_order_seq_cst);
        }
        std::memcpy(memcpyDst, memcpySrc, size);
    }

    TThreadResult RunWorker(
        const TConfig& config,
        ui32 index,
        i32 cpu,
        std::atomic<ui32>& ready,
        std::atomic<bool>& start,
        std::atomic<ui64>* sharedCounter)
    {
        PinThread(cpu);

        const ui64 slotCount = Max<ui64>(1, (config.WorkingSetBytes + config.SizeBytes - 1) / config.SizeBytes);
        const ui64 actualWorkingSetBytes = slotCount * config.SizeBytes;

        TVector<ui8> src(actualWorkingSetBytes);
        TVector<ui8> memcpyDst(actualWorkingSetBytes);
        TVector<ui8> memsetDst(actualWorkingSetBytes);

        FillRandom(src, config.Seed + index * 0x9e3779b97f4a7c15ULL);
        std::memset(memcpyDst.data(), 0, memcpyDst.size());
        std::memset(memsetDst.data(), 0, memsetDst.size());

        ui64 slot = 0;
        const auto warmupDeadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(config.WarmupSec);
        while (std::chrono::steady_clock::now() < warmupDeadline) {
            const ui64 offset = slot * config.SizeBytes;
            DoOps(
                memsetDst.data() + offset,
                memcpyDst.data() + offset,
                src.data() + offset,
                config.SizeBytes,
                static_cast<ui8>(slot),
                sharedCounter);
            slot = (slot + 1) % slotCount;
        }

        slot = 0;
        ready.fetch_add(1, std::memory_order_release);
        while (!start.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }

        const auto startTime = std::chrono::steady_clock::now();
        const auto deadline = startTime + std::chrono::duration<double>(config.DurationSec);
        const ui64 startCycles = GetCycleCount();
        ui64 calls = 0;

        while (std::chrono::steady_clock::now() < deadline) {
            const ui64 offset = slot * config.SizeBytes;
            DoOps(
                memsetDst.data() + offset,
                memcpyDst.data() + offset,
                src.data() + offset,
                config.SizeBytes,
                static_cast<ui8>(calls + slot),
                sharedCounter);
            slot = (slot + 1) % slotCount;
            ++calls;
        }

        const ui64 finishCycles = GetCycleCount();
        const auto finishTime = std::chrono::steady_clock::now();
        const double seconds = std::chrono::duration<double>(finishTime - startTime).count();
        const ui64 finalOffset = ((slot + slotCount - 1) % slotCount) * config.SizeBytes;
        ui64 sink =
            static_cast<ui64>(memcpyDst[finalOffset]) |
            (static_cast<ui64>(memsetDst[finalOffset]) << 8);
        Y_DO_NOT_OPTIMIZE_AWAY(sink);

        return {
            .Calls = calls,
            .PayloadBytes = calls * config.SizeBytes,
            .TrafficBytes = calls * config.SizeBytes * 3,
            .Cycles = finishCycles - startCycles,
            .Seconds = seconds,
            .Sink = sink,
            .Cpu = CurrentCpu(),
        };
    }

    void PrintThreadResult(ui32 index, const TThreadResult& result) {
        const double payloadBytesPerSec = result.PayloadBytes / result.Seconds;
        const double trafficBytesPerSec = result.TrafficBytes / result.Seconds;

        Cout
            << "thread=" << index
            << " cpu=" << result.Cpu
            << " calls=" << result.Calls
            << " payload_bytes=" << result.PayloadBytes
            << " traffic_bytes=" << result.TrafficBytes
            << " seconds=" << result.Seconds
            << " payload_gb_per_sec=" << (payloadBytesPerSec / 1e9)
            << " traffic_gb_per_sec=" << (trafficBytesPerSec / 1e9)
            << " cycles_per_call=" << (static_cast<double>(result.Cycles) / Max<ui64>(1, result.Calls))
            << " cycles_per_traffic_byte=" << (static_cast<double>(result.Cycles) / Max<ui64>(1, result.TrafficBytes))
            << " sink=" << result.Sink
            << Endl;
    }

} // namespace

int main(int argc, char** argv) {
    TConfig config;

    NLastGetopt::TOpts opts = NLastGetopt::TOpts::Default();
    opts.AddLongOption("size", "Bytes touched by one memset and one memcpy")
        .RequiredArgument("BYTES")
        .StoreResult(&config.SizeBytes, config.SizeBytes);
    opts.AddLongOption("working-set-bytes", "Total per-thread data set cycled by the benchmark")
        .RequiredArgument("BYTES")
        .StoreResult(&config.WorkingSetBytes, config.WorkingSetBytes);
    opts.AddLongOption('t', "threads", "Benchmark threads")
        .RequiredArgument("NUM")
        .StoreResult(&config.Threads, config.Threads);
    opts.AddLongOption("warmup-sec", "Warmup time per thread in seconds")
        .RequiredArgument("SEC")
        .StoreResult(&config.WarmupSec, config.WarmupSec);
    opts.AddLongOption("duration-sec", "Measured time per thread in seconds")
        .RequiredArgument("SEC")
        .StoreResult(&config.DurationSec, config.DurationSec);
    opts.AddLongOption("seed", "Deterministic fill seed")
        .RequiredArgument("NUM")
        .StoreResult(&config.Seed, config.Seed);
    opts.AddLongOption("cpu-list", "Comma-separated CPU list/ranges; thread i is pinned to the i-th expanded CPU")
        .RequiredArgument("LIST")
        .StoreResult(&config.CpuList, config.CpuList);
    opts.AddLongOption("shared-seq-cst-atomic-before-memcpy", "Increment one shared seq_cst atomic counter before every memcpy")
        .NoArgument()
        .SetFlag(&config.SharedSeqCstAtomicBeforeMemcpy);

    NLastGetopt::TOptsParseResult parseResult(&opts, argc, argv);
    Y_UNUSED(parseResult);

    Y_ABORT_UNLESS(config.SizeBytes > 0, "size must be > 0");
    Y_ABORT_UNLESS(config.WorkingSetBytes > 0, "working-set-bytes must be > 0");
    Y_ABORT_UNLESS(config.Threads > 0, "threads must be > 0");
    Y_ABORT_UNLESS(config.DurationSec > 0, "duration-sec must be > 0");
    Y_ABORT_UNLESS(config.WarmupSec >= 0, "warmup-sec must be >= 0");

    const TVector<i32> cpus = ParseCpuList(config.CpuList);
    if (!cpus.empty()) {
        Y_ABORT_UNLESS(cpus.size() >= config.Threads, "cpu-list contains %zu CPUs, but %u threads requested", cpus.size(), config.Threads);
    }

    const ui64 slotCount = Max<ui64>(1, (config.WorkingSetBytes + config.SizeBytes - 1) / config.SizeBytes);
    const ui64 actualWorkingSetBytes = slotCount * config.SizeBytes;

    Cout
        << "mem_bw_stress"
        << " size=" << config.SizeBytes
        << " working_set_bytes=" << actualWorkingSetBytes
        << " slots=" << slotCount
        << " threads=" << config.Threads
        << " warmup_sec=" << config.WarmupSec
        << " duration_sec=" << config.DurationSec
        << " cpu_list=" << config.CpuList
        << " shared_seq_cst_atomic_before_memcpy=" << static_cast<ui32>(config.SharedSeqCstAtomicBeforeMemcpy)
        << " cycles_per_second=" << NHPTimer::GetCyclesPerSecond()
        << Endl;

    std::atomic<ui32> ready = 0;
    std::atomic<bool> start = false;
    std::atomic<ui64> sharedCounter = 0;
    std::atomic<ui64>* sharedCounterPtr = config.SharedSeqCstAtomicBeforeMemcpy ? &sharedCounter : nullptr;
    TVector<TThreadResult> results(config.Threads);
    TVector<std::thread> threads;
    threads.reserve(config.Threads);

    for (ui32 i = 0; i < config.Threads; ++i) {
        const i32 cpu = cpus.empty() ? -1 : cpus[i];
        threads.emplace_back([&, i, cpu]() {
            results[i] = RunWorker(config, i, cpu, ready, start, sharedCounterPtr);
        });
    }

    while (ready.load(std::memory_order_acquire) != config.Threads) {
        std::this_thread::yield();
    }

    sharedCounter.store(0, std::memory_order_relaxed);
    const auto globalStart = std::chrono::steady_clock::now();
    start.store(true, std::memory_order_release);

    for (auto& thread : threads) {
        thread.join();
    }

    const auto globalFinish = std::chrono::steady_clock::now();
    const double totalSeconds = std::chrono::duration<double>(globalFinish - globalStart).count();

    ui64 totalCalls = 0;
    ui64 totalPayloadBytes = 0;
    ui64 totalTrafficBytes = 0;
    ui64 totalCycles = 0;
    ui64 sinkXor = 0;

    for (ui32 i = 0; i < config.Threads; ++i) {
        const auto& result = results[i];
        totalCalls += result.Calls;
        totalPayloadBytes += result.PayloadBytes;
        totalTrafficBytes += result.TrafficBytes;
        totalCycles += result.Cycles;
        sinkXor ^= result.Sink;
        PrintThreadResult(i, result);
    }

    const double payloadBytesPerSec = totalPayloadBytes / totalSeconds;
    const double trafficBytesPerSec = totalTrafficBytes / totalSeconds;

    Cout
        << "aggregate"
        << " threads=" << config.Threads
        << " calls=" << totalCalls
        << " payload_bytes=" << totalPayloadBytes
        << " traffic_bytes=" << totalTrafficBytes
        << " seconds=" << totalSeconds
        << " payload_gb_per_sec=" << (payloadBytesPerSec / 1e9)
        << " traffic_gb_per_sec=" << (trafficBytesPerSec / 1e9)
        << " cycles_per_call=" << (static_cast<double>(totalCycles) / Max<ui64>(1, totalCalls))
        << " cycles_per_traffic_byte=" << (static_cast<double>(totalCycles) / Max<ui64>(1, totalTrafficBytes))
        << " shared_counter=" << sharedCounter.load(std::memory_order_relaxed)
        << " sink_xor=" << sinkXor
        << Endl;

    return 0;
}
