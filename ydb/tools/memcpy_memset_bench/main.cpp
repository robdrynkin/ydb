#include <library/cpp/getopt/last_getopt.h>

#include <util/generic/vector.h>
#include <util/generic/ymath.h>
#include <util/stream/output.h>
#include <util/system/compiler.h>
#include <util/system/datetime.h>
#include <util/system/hp_timer.h>
#include <util/system/types.h>

#include <chrono>
#include <cstring>

#ifdef _linux_
#include <pthread.h>
#include <sched.h>
#endif

namespace {

    struct TConfig {
        ui64 SizeBytes = 2686;
        ui64 WorkingSetBytes = 2686;
        ui64 Iterations = 5'000'000;
        ui64 WarmupIterations = 100'000;
        ui64 Seed = 1;
        i32 Cpu = -1;
    };

    struct TResult {
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

    Y_FORCE_INLINE void DoOps(ui8* memsetDst, ui8* memcpyDst, const ui8* memcpySrc, size_t size, ui8 memsetValue) {
        std::memset(memsetDst, memsetValue, size);
        std::memcpy(memcpyDst, memcpySrc, size);
    }

    TResult Run(const TConfig& config) {
        PinThread(config.Cpu);

        const ui64 slotCount = Max<ui64>(1, (config.WorkingSetBytes + config.SizeBytes - 1) / config.SizeBytes);
        const ui64 actualWorkingSetBytes = slotCount * config.SizeBytes;

        TVector<ui8> src(actualWorkingSetBytes);
        TVector<ui8> memcpyDst(actualWorkingSetBytes);
        TVector<ui8> memsetDst(actualWorkingSetBytes);

        FillRandom(src, config.Seed);
        std::memset(memcpyDst.data(), 0, memcpyDst.size());
        std::memset(memsetDst.data(), 0, memsetDst.size());

        ui64 slot = 0;
        for (ui64 i = 0; i < config.WarmupIterations; ++i) {
            const ui64 offset = slot * config.SizeBytes;
            DoOps(
                memsetDst.data() + offset,
                memcpyDst.data() + offset,
                src.data() + offset,
                config.SizeBytes,
                static_cast<ui8>(i + slot));
            slot = (slot + 1) % slotCount;
        }

        slot = 0;
        const auto startTime = std::chrono::steady_clock::now();
        const ui64 startCycles = GetCycleCount();

        for (ui64 i = 0; i < config.Iterations; ++i) {
            const ui64 offset = slot * config.SizeBytes;
            DoOps(
                memsetDst.data() + offset,
                memcpyDst.data() + offset,
                src.data() + offset,
                config.SizeBytes,
                static_cast<ui8>(i + slot));
            slot = (slot + 1) % slotCount;
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
            .Calls = config.Iterations,
            .PayloadBytes = config.Iterations * config.SizeBytes,
            .TrafficBytes = config.Iterations * config.SizeBytes * 3,
            .Cycles = finishCycles - startCycles,
            .Seconds = seconds,
            .Sink = sink,
            .Cpu = CurrentCpu(),
        };
    }

} // namespace

int main(int argc, char** argv) {
    TConfig config;

    NLastGetopt::TOpts opts = NLastGetopt::TOpts::Default();
    opts.AddLongOption("size", "Bytes touched by one memset and one memcpy")
        .RequiredArgument("BYTES")
        .StoreResult(&config.SizeBytes, config.SizeBytes);
    opts.AddLongOption("working-set-bytes", "Total per-thread data set cycled by the benchmark; == size approximates hot-buffer mode")
        .RequiredArgument("BYTES")
        .StoreResult(&config.WorkingSetBytes, config.WorkingSetBytes);
    opts.AddLongOption("iterations", "Measured iterations")
        .RequiredArgument("NUM")
        .StoreResult(&config.Iterations, config.Iterations);
    opts.AddLongOption("warmup-iterations", "Warmup iterations before the measurement")
        .RequiredArgument("NUM")
        .StoreResult(&config.WarmupIterations, config.WarmupIterations);
    opts.AddLongOption("seed", "Deterministic fill seed")
        .RequiredArgument("NUM")
        .StoreResult(&config.Seed, config.Seed);
    opts.AddLongOption("cpu", "Pin the benchmark thread to this CPU; negative disables pinning")
        .RequiredArgument("CPU")
        .StoreResult(&config.Cpu, config.Cpu);

    NLastGetopt::TOptsParseResult parseResult(&opts, argc, argv);
    Y_UNUSED(parseResult);

    Y_ABORT_UNLESS(config.SizeBytes > 0, "size must be > 0");
    Y_ABORT_UNLESS(config.WorkingSetBytes > 0, "working-set-bytes must be > 0");
    Y_ABORT_UNLESS(config.Iterations > 0, "iterations must be > 0");

    const ui64 slotCount = Max<ui64>(1, (config.WorkingSetBytes + config.SizeBytes - 1) / config.SizeBytes);
    const ui64 actualWorkingSetBytes = slotCount * config.SizeBytes;

    Cout
        << "memcpy_memset_bench"
        << " size=" << config.SizeBytes
        << " working_set_bytes=" << actualWorkingSetBytes
        << " slots=" << slotCount
        << " iterations=" << config.Iterations
        << " warmup_iterations=" << config.WarmupIterations
        << " cpu=" << config.Cpu
        << " cycles_per_second=" << NHPTimer::GetCyclesPerSecond()
        << Endl;

    const TResult result = Run(config);

    const double payloadBytesPerSec = result.PayloadBytes / result.Seconds;
    const double trafficBytesPerSec = result.TrafficBytes / result.Seconds;
    const double nsPerCall = result.Seconds * 1e9 / result.Calls;
    const double cyclesPerCall = static_cast<double>(result.Cycles) / result.Calls;
    const double cyclesPerPayloadByte = static_cast<double>(result.Cycles) / result.PayloadBytes;
    const double cyclesPerTrafficByte = static_cast<double>(result.Cycles) / result.TrafficBytes;

    Cout
        << "thread=0"
        << " cpu=" << result.Cpu
        << " calls=" << result.Calls
        << " payload_bytes=" << result.PayloadBytes
        << " traffic_bytes=" << result.TrafficBytes
        << " seconds=" << result.Seconds
        << " payload_gb_per_sec=" << (payloadBytesPerSec / 1e9)
        << " traffic_gb_per_sec=" << (trafficBytesPerSec / 1e9)
        << " ns_per_call=" << nsPerCall
        << " cycles_per_call=" << cyclesPerCall
        << " cycles_per_payload_byte=" << cyclesPerPayloadByte
        << " cycles_per_traffic_byte=" << cyclesPerTrafficByte
        << " sink=" << result.Sink
        << Endl;

    return 0;
}
