#define XXH_INLINE_ALL
#include <contrib/libs/xxhash/xxhash.h>

#include <library/cpp/getopt/last_getopt.h>

#include <util/generic/vector.h>
#include <util/generic/string.h>
#include <util/generic/ymath.h>
#include <util/stream/output.h>
#include <util/system/compiler.h>
#include <util/system/datetime.h>
#include <util/system/hp_timer.h>
#include <util/system/types.h>

#include <atomic>
#include <chrono>
#include <thread>

#ifdef _linux_
#include <sched.h>
#include <pthread.h>
#endif

namespace {

    struct TConfig {
        ui64 SizeBytes = 2686;
        ui64 WorkingSetBytes = 2686;
        ui64 Iterations = 5'000'000;
        ui64 WarmupIterations = 100'000;
        ui32 Threads = 1;
        ui64 Seed = 1;
        i32 CpuBase = -1;
        ui32 CpuStride = 1;
        bool ResetEachIter = false;
    };

    struct TThreadResult {
        ui64 Calls = 0;
        ui64 Bytes = 0;
        ui64 Cycles = 0;
        double Seconds = 0.0;
        XXH64_hash_t Digest = 0;
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

    Y_FORCE_INLINE void DoUpdate(XXH3_state_t& state, const ui8* data, size_t size, bool resetEachIter) {
        if (resetEachIter) {
            XXH3_64bits_reset(&state);
        }
        XXH3_64bits_update(&state, data, size);
    }

    TThreadResult RunWorker(const TConfig& config, ui32 index, std::atomic<ui32>& ready, std::atomic<bool>& start) {
        const i32 cpu = config.CpuBase >= 0 ? config.CpuBase + static_cast<i32>(index * config.CpuStride) : -1;
        PinThread(cpu);

        const ui64 slotCount = Max<ui64>(1, (config.WorkingSetBytes + config.SizeBytes - 1) / config.SizeBytes);
        const ui64 actualWorkingSetBytes = slotCount * config.SizeBytes;
        TVector<ui8> buffer(actualWorkingSetBytes);
        FillRandom(buffer, config.Seed + index * 0x9e3779b97f4a7c15ULL);

        XXH3_state_t state;
        Y_ABORT_UNLESS(XXH3_64bits_reset(&state) == XXH_OK);

        ui64 slot = 0;
        for (ui64 i = 0; i < config.WarmupIterations; ++i) {
            const ui8* data = buffer.data() + slot * config.SizeBytes;
            DoUpdate(state, data, config.SizeBytes, config.ResetEachIter);
            slot = (slot + 1) % slotCount;
        }

        Y_ABORT_UNLESS(XXH3_64bits_reset(&state) == XXH_OK);
        slot = 0;

        ready.fetch_add(1, std::memory_order_release);
        while (!start.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }

        const auto startTime = std::chrono::steady_clock::now();
        const ui64 startCycles = GetCycleCount();

        for (ui64 i = 0; i < config.Iterations; ++i) {
            const ui8* data = buffer.data() + slot * config.SizeBytes;
            DoUpdate(state, data, config.SizeBytes, config.ResetEachIter);
            slot = (slot + 1) % slotCount;
        }

        const ui64 finishCycles = GetCycleCount();
        const auto finishTime = std::chrono::steady_clock::now();
        const double seconds = std::chrono::duration<double>(finishTime - startTime).count();
        auto digest = XXH3_64bits_digest(&state);
        Y_DO_NOT_OPTIMIZE_AWAY(digest);

        return {
            .Calls = config.Iterations,
            .Bytes = config.Iterations * config.SizeBytes,
            .Cycles = finishCycles - startCycles,
            .Seconds = seconds,
            .Digest = digest,
            .Cpu = CurrentCpu(),
        };
    }

    void PrintThreadResult(ui32 index, const TThreadResult& result) {
        const double callsPerSec = result.Calls / result.Seconds;
        const double bytesPerSec = result.Bytes / result.Seconds;
        const double nsPerCall = result.Seconds * 1e9 / result.Calls;
        const double cyclesPerCall = static_cast<double>(result.Cycles) / result.Calls;
        const double cyclesPerByte = static_cast<double>(result.Cycles) / result.Bytes;

        Cout
            << "thread=" << index
            << " cpu=" << result.Cpu
            << " calls=" << result.Calls
            << " bytes=" << result.Bytes
            << " seconds=" << result.Seconds
            << " calls_per_sec=" << callsPerSec
            << " gb_per_sec=" << (bytesPerSec / 1e9)
            << " ns_per_call=" << nsPerCall
            << " cycles_per_call=" << cyclesPerCall
            << " cycles_per_byte=" << cyclesPerByte
            << " digest=" << result.Digest
            << Endl;
    }

} // namespace

int main(int argc, char** argv) {
    TConfig config;

    NLastGetopt::TOpts opts = NLastGetopt::TOpts::Default();
    opts.AddLongOption("size", "Bytes passed to one XXH3_64bits_update call")
        .RequiredArgument("BYTES")
        .StoreResult(&config.SizeBytes, config.SizeBytes);
    opts.AddLongOption("working-set-bytes", "Total per-thread data set cycled by the benchmark; == size approximates hot-buffer mode")
        .RequiredArgument("BYTES")
        .StoreResult(&config.WorkingSetBytes, config.WorkingSetBytes);
    opts.AddLongOption("iterations", "Measured iterations per thread")
        .RequiredArgument("NUM")
        .StoreResult(&config.Iterations, config.Iterations);
    opts.AddLongOption("warmup-iterations", "Warmup iterations per thread before the measurement")
        .RequiredArgument("NUM")
        .StoreResult(&config.WarmupIterations, config.WarmupIterations);
    opts.AddLongOption('t', "threads", "Benchmark threads")
        .RequiredArgument("NUM")
        .StoreResult(&config.Threads, config.Threads);
    opts.AddLongOption("seed", "Deterministic fill seed")
        .RequiredArgument("NUM")
        .StoreResult(&config.Seed, config.Seed);
    opts.AddLongOption("cpu-base", "Pin thread i to cpu-base + i * cpu-stride; negative disables pinning")
        .RequiredArgument("CPU")
        .StoreResult(&config.CpuBase, config.CpuBase);
    opts.AddLongOption("cpu-stride", "Stride between pinned CPUs")
        .RequiredArgument("NUM")
        .StoreResult(&config.CpuStride, config.CpuStride);
    opts.AddLongOption("reset-each-iter", "Reset XXH3 state before each update")
        .NoArgument()
        .SetFlag(&config.ResetEachIter);

    NLastGetopt::TOptsParseResult parseResult(&opts, argc, argv);
    Y_UNUSED(parseResult);

    Y_ABORT_UNLESS(config.SizeBytes > 0, "size must be > 0");
    Y_ABORT_UNLESS(config.WorkingSetBytes > 0, "working-set-bytes must be > 0");
    Y_ABORT_UNLESS(config.Iterations > 0, "iterations must be > 0");
    Y_ABORT_UNLESS(config.Threads > 0, "threads must be > 0");
    Y_ABORT_UNLESS(config.CpuStride > 0, "cpu-stride must be > 0");

    const ui64 slotCount = Max<ui64>(1, (config.WorkingSetBytes + config.SizeBytes - 1) / config.SizeBytes);
    const ui64 actualWorkingSetBytes = slotCount * config.SizeBytes;

    Cout
        << "xxh3_update_bench"
        << " size=" << config.SizeBytes
        << " working_set_bytes=" << actualWorkingSetBytes
        << " slots=" << slotCount
        << " iterations=" << config.Iterations
        << " warmup_iterations=" << config.WarmupIterations
        << " threads=" << config.Threads
        << " reset_each_iter=" << static_cast<int>(config.ResetEachIter)
        << " cpu_base=" << config.CpuBase
        << " cpu_stride=" << config.CpuStride
        << " cycles_per_second=" << NHPTimer::GetCyclesPerSecond()
        << Endl;

    std::atomic<ui32> ready = 0;
    std::atomic<bool> start = false;
    TVector<TThreadResult> results(config.Threads);
    TVector<std::thread> threads;
    threads.reserve(config.Threads);

    for (ui32 i = 0; i < config.Threads; ++i) {
        threads.emplace_back([&, i]() {
            results[i] = RunWorker(config, i, ready, start);
        });
    }

    while (ready.load(std::memory_order_acquire) != config.Threads) {
        std::this_thread::yield();
    }

    const auto globalStart = std::chrono::steady_clock::now();
    start.store(true, std::memory_order_release);

    for (auto& thread : threads) {
        thread.join();
    }
    const auto globalFinish = std::chrono::steady_clock::now();

    ui64 totalCalls = 0;
    ui64 totalBytes = 0;
    ui64 totalCycles = 0;
    XXH64_hash_t digestXor = 0;
    double maxThreadSeconds = 0.0;
    double avgThreadSeconds = 0.0;

    for (ui32 i = 0; i < config.Threads; ++i) {
        PrintThreadResult(i, results[i]);
        totalCalls += results[i].Calls;
        totalBytes += results[i].Bytes;
        totalCycles += results[i].Cycles;
        digestXor ^= results[i].Digest;
        maxThreadSeconds = Max(maxThreadSeconds, results[i].Seconds);
        avgThreadSeconds += results[i].Seconds;
    }
    avgThreadSeconds /= config.Threads;

    const double globalSeconds = std::chrono::duration<double>(globalFinish - globalStart).count();
    const double totalCallsPerSec = totalCalls / globalSeconds;
    const double totalBytesPerSec = totalBytes / globalSeconds;

    Cout
        << "aggregate"
        << " calls=" << totalCalls
        << " bytes=" << totalBytes
        << " wall_seconds=" << globalSeconds
        << " calls_per_sec=" << totalCallsPerSec
        << " gb_per_sec=" << (totalBytesPerSec / 1e9)
        << " avg_thread_seconds=" << avgThreadSeconds
        << " max_thread_seconds=" << maxThreadSeconds
        << " avg_cycles_per_call=" << (static_cast<double>(totalCycles) / totalCalls)
        << " avg_cycles_per_byte=" << (static_cast<double>(totalCycles) / totalBytes)
        << " digest_xor=" << digestXor
        << Endl;

    return 0;
}
