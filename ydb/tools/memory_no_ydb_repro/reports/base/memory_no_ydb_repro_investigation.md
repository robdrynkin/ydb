# Memory No-YDB Reproduction Investigation

Last updated: 2026-03-12

## Scope

Goal: reproduce the observed memory-speed degradation on the hosts without running YDB itself.

This first step establishes achievable memory throughput against the platform spec and checks whether a purely synthetic load can degrade local NUMA memory speed.

## Host/Memory Spec Reference

- platform: `AMD EPYC 9654`
- per socket / per NUMA node: `12 x DDR5-4800` channels
- theoretical local NUMA peak: `460.8 GB/s`
- theoretical socket-to-socket fabric upper bound: about `256 GB/s` one-way
- last-level cache domain: `32 MiB` per CCD

## Benchmark Setup

- hosts: `1302`, `1306`, `1308`
- YDB disabled during these experiments
- benchmark: `ydb/tools/memory_no_ydb_repro/mem_bw_stress`
- operation mix: one `memset` + one `memcpy` per iteration
- access size: `4096 B`
- per-thread working set: `64 MiB`
- threads per process: `96`
- CPUs: physical cores only
  - NUMA 0 CPUs: `0-95`
  - NUMA 1 CPUs: `96-191`
- warmup: `1 s`
- measured window: `5 s`
- counters: `AMDuProfPcm --msr -m memory -a -A system -d 5 -C`

The synthetic benchmark reports two bandwidth numbers:

- `payload_gb_per_sec`: useful payload bytes (`size` per iteration)
- `traffic_gb_per_sec`: modeled memory traffic = `3 * size` per iteration (read source + memcpy write + memset write)

## Scenarios

- `local_numa0`: CPUs on NUMA 0, memory on NUMA 0
- `local_numa1`: CPUs on NUMA 1, memory on NUMA 1
- `remote_0_to_1`: CPUs on NUMA 0, memory on NUMA 1
- `remote_1_to_0`: CPUs on NUMA 1, memory on NUMA 0
- `dual_local`: two simultaneous processes, each local to its own NUMA
- `dual_cross`: two simultaneous processes, each remote to the opposite NUMA
- `mixed_target_0`: one local process on NUMA 0 + one remote process from NUMA 1, both targeting NUMA 0 memory
- `mixed_target_1`: one local process on NUMA 1 + one remote process from NUMA 0, both targeting NUMA 1 memory

## Results: Aggregate Throughput

| Host | Scenario | Bench traffic GB/s | uProf total BW GB/s | uProf remote share |
|---|---|---:|---:|---:|
| node-1302 | local_numa0 | 213.7 | 313.62 | 0.1% |
| node-1302 | local_numa1 | 213.9 | 311.20 | 0.1% |
| node-1302 | remote_0_to_1 | 110.2 | 167.46 | 99.7% |
| node-1302 | remote_1_to_0 | 109.7 | 183.80 | 99.8% |
| node-1302 | dual_local | 422.8 | 615.57 | 0.1% |
| node-1302 | dual_cross | 136.0 | 224.61 | 99.8% |
| node-1302 | mixed_target_0 | 223.5 | 359.16 | 25.1% |
| node-1302 | mixed_target_1 | 229.1 | 360.00 | 23.6% |
| node-1306 | local_numa0 | 213.9 | 316.11 | 0.1% |
| node-1306 | local_numa1 | 213.7 | 315.03 | 0.1% |
| node-1306 | remote_0_to_1 | 110.2 | 168.57 | 99.8% |
| node-1306 | remote_1_to_0 | 109.3 | 161.59 | 99.7% |
| node-1306 | dual_local | 423.0 | 614.85 | 0.1% |
| node-1306 | dual_cross | 135.2 | 222.33 | 99.8% |
| node-1306 | mixed_target_0 | 222.8 | 361.19 | 25.4% |
| node-1306 | mixed_target_1 | 220.3 | 372.17 | 26.3% |
| node-1308 | local_numa0 | 214.7 | 315.57 | 0.1% |
| node-1308 | local_numa1 | 213.8 | 311.10 | 0.1% |
| node-1308 | remote_0_to_1 | 110.3 | 167.80 | 99.7% |
| node-1308 | remote_1_to_0 | 109.7 | 165.59 | 99.7% |
| node-1308 | dual_local | 422.7 | 615.01 | 0.0% |
| node-1308 | dual_cross | 135.3 | 222.73 | 99.8% |
| node-1308 | mixed_target_0 | 226.4 | 360.55 | 24.3% |
| node-1308 | mixed_target_1 | 227.0 | 362.34 | 24.2% |

## Results: Per-Process Breakdown For The Interference Scenarios

| Host | Scenario | Process | CPU set | Memory node | Bench traffic GB/s |
|---|---|---:|---|---:|---:|
| node-1302 | dual_local | 0 | `0-95` | 0 | 211.3 |
| node-1302 | dual_local | 1 | `96-191` | 1 | 211.4 |
| node-1302 | dual_cross | 0 | `0-95` | 1 | 67.1 |
| node-1302 | dual_cross | 1 | `96-191` | 0 | 68.9 |
| node-1302 | mixed_target_0 | 0 | `0-95` | 0 | 161.3 |
| node-1302 | mixed_target_0 | 1 | `96-191` | 0 | 62.2 |
| node-1302 | mixed_target_1 | 0 | `0-95` | 1 | 65.7 |
| node-1302 | mixed_target_1 | 1 | `96-191` | 1 | 163.4 |
| node-1306 | dual_local | 0 | `0-95` | 0 | 211.8 |
| node-1306 | dual_local | 1 | `96-191` | 1 | 211.3 |
| node-1306 | dual_cross | 0 | `0-95` | 1 | 66.8 |
| node-1306 | dual_cross | 1 | `96-191` | 0 | 68.4 |
| node-1306 | mixed_target_0 | 0 | `0-95` | 0 | 161.6 |
| node-1306 | mixed_target_0 | 1 | `96-191` | 0 | 61.2 |
| node-1306 | mixed_target_1 | 0 | `0-95` | 1 | 60.0 |
| node-1306 | mixed_target_1 | 1 | `96-191` | 1 | 160.3 |
| node-1308 | dual_local | 0 | `0-95` | 0 | 211.3 |
| node-1308 | dual_local | 1 | `96-191` | 1 | 211.4 |
| node-1308 | dual_cross | 0 | `0-95` | 1 | 66.6 |
| node-1308 | dual_cross | 1 | `96-191` | 0 | 68.7 |
| node-1308 | mixed_target_0 | 0 | `0-95` | 0 | 162.1 |
| node-1308 | mixed_target_0 | 1 | `96-191` | 0 | 64.3 |
| node-1308 | mixed_target_1 | 0 | `0-95` | 1 | 63.5 |
| node-1308 | mixed_target_1 | 1 | `96-191` | 1 | 163.5 |

## Key Observations

1. Single-NUMA local throughput is stable and very similar on all three hosts.
   - node-1302: local_numa0 `213.7` GB/s bench / `313.62` GB/s uProf, local_numa1 `213.9` GB/s bench / `311.20` GB/s uProf
   - node-1306: local_numa0 `213.9` GB/s bench / `316.11` GB/s uProf, local_numa1 `213.7` GB/s bench / `315.03` GB/s uProf
   - node-1308: local_numa0 `214.7` GB/s bench / `315.57` GB/s uProf, local_numa1 `213.8` GB/s bench / `311.10` GB/s uProf

2. Pure cross-NUMA traffic is much slower than local traffic.
   - node-1302: remote single-process bench traffic `110.2` / `109.7` GB/s, uProf total `167.46` / `183.80` GB/s, remote share ~100%
   - node-1306: remote single-process bench traffic `110.2` / `109.3` GB/s, uProf total `168.57` / `161.59` GB/s, remote share ~100%
   - node-1308: remote single-process bench traffic `110.3` / `109.7` GB/s, uProf total `167.80` / `165.59` GB/s, remote share ~100%

3. Two local NUMA processes scale almost linearly:
   - node-1302: dual_local `422.8` GB/s bench aggregate, `615.57` GB/s uProf total
   - node-1306: dual_local `423.0` GB/s bench aggregate, `614.85` GB/s uProf total
   - node-1308: dual_local `422.7` GB/s bench aggregate, `615.01` GB/s uProf total

4. The first clean non-YDB reproducer of degradation is `mixed_target_*`: one local process plus one remote process targeting the same NUMA memory.
   - node-1302: local NUMA0 throughput drops from `213.7` to `161.3` GB/s (`-24.5%`) when a remote NUMA1 process also targets NUMA0 memory; local NUMA1 throughput drops from `213.9` to `163.4` GB/s (`-23.6%`) under the symmetric scenario.
   - node-1306: local NUMA0 throughput drops from `213.9` to `161.6` GB/s (`-24.5%`) when a remote NUMA1 process also targets NUMA0 memory; local NUMA1 throughput drops from `213.7` to `160.3` GB/s (`-25.0%`) under the symmetric scenario.
   - node-1308: local NUMA0 throughput drops from `214.7` to `162.1` GB/s (`-24.5%`) when a remote NUMA1 process also targets NUMA0 memory; local NUMA1 throughput drops from `213.8` to `163.5` GB/s (`-23.5%`) under the symmetric scenario.

5. In the mixed scenarios, the remote process still gets only about `60-66 GB/s` of traffic, but that is enough to cut the local process by about a quarter. This is exactly the kind of interference needed for a minimal reproduction.

## Practical Conclusion

The hosts do not need YDB to demonstrate the key memory effect. A minimal synthetic reproducer already exists:

- run one `mem_bw_stress` process on local NUMA memory
- run a second `mem_bw_stress` process from the opposite NUMA, but bind its memory to the same target NUMA
- observe that the local process loses about `23-25%` of its memory throughput, even though the competing process is only the remote contributor

This is the cleanest next base for building a smaller reproducer and then layering scheduler / IPI / other effects on top if needed.

## Artifacts

- raw matrix: `/home/robdrynkin/ydbwork/ydb/ydb/tools/memory_no_ydb_repro/reports/memory_no_ydb_matrix.json`
- remote-run log for this series: `/home/robdrynkin/ydbwork/ydb/ydb/tools/memory_no_ydb_repro/reports/memory_no_ydb_repro_investigation.md`

## Thread-Count Sweep: 4 KiB vs 1 MiB

### Setup

This second pass reran the same eight NUMA scenarios on `1302`, `1306`, and `1308`, but swept the number of threads per process:

- thread counts: `1, 2, 4, 8, 12, 16, 24, 32, 48, 64, 96`
- sizes: `4 KiB` and `1 MiB`
- working set per thread: `64 MiB`
- measured window: `3 s`
- prewarm before the measured window: `1 s`
- CPUs were taken from physical cores only and spread across CCDs in an interleaved order inside each NUMA node, instead of using only the first contiguous cores
- `AMDuProfPcm --msr -m memory -a -A system -d 3 -C` was run in parallel for every point

Raw sweep output:

- `/home/robdrynkin/ydbwork/ydb/ydb/tools/memory_no_ydb_repro/reports/memory_no_ydb_sweep.json`

Generated graphs:

- `/home/robdrynkin/ydbwork/ydb/ydb/tools/memory_no_ydb_repro/reports/mem_bw_sweep_bench_4KiB.svg`
- `/home/robdrynkin/ydbwork/ydb/ydb/tools/memory_no_ydb_repro/reports/mem_bw_sweep_bench_1MiB.svg`
- `/home/robdrynkin/ydbwork/ydb/ydb/tools/memory_no_ydb_repro/reports/mem_bw_sweep_uprof_4KiB.svg`
- `/home/robdrynkin/ydbwork/ydb/ydb/tools/memory_no_ydb_repro/reports/mem_bw_sweep_uprof_1MiB.svg`
- `/home/robdrynkin/ydbwork/ydb/ydb/tools/memory_no_ydb_repro/reports/mem_bw_sweep_components_mixed_target_0_4KiB.svg`
- `/home/robdrynkin/ydbwork/ydb/ydb/tools/memory_no_ydb_repro/reports/mem_bw_sweep_components_mixed_target_1_4KiB.svg`
- `/home/robdrynkin/ydbwork/ydb/ydb/tools/memory_no_ydb_repro/reports/mem_bw_sweep_components_mixed_target_0_1MiB.svg`
- `/home/robdrynkin/ydbwork/ydb/ydb/tools/memory_no_ydb_repro/reports/mem_bw_sweep_components_mixed_target_1_1MiB.svg`

### Aggregate Sweep Summary

Values below are means across `1302`, `1306`, and `1308`.

#### 4 KiB

| Scenario | Peak GB/s | Peak threads | 95% of peak at | 1 thread | 8 threads | 12 threads | 96 threads |
|---|---:|---:|---:|---:|---:|---:|---:|
| local_numa0 | 235.49 | 12 | 12 | 36.98 | 197.24 | 235.49 | 213.36 |
| local_numa1 | 235.40 | 12 | 12 | 36.68 | 197.82 | 235.40 | 213.16 |
| remote_0_to_1 | 116.57 | 12 | 8 | 22.33 | 112.88 | 116.57 | 110.06 |
| remote_1_to_0 | 114.79 | 12 | 8 | 22.27 | 111.33 | 114.79 | 109.42 |
| dual_local | 468.45 | 12 | 12 | 73.53 | 393.36 | 468.45 | 421.73 |
| dual_cross | 150.41 | 8 | 8 | 44.57 | 150.41 | 149.67 | 136.75 |
| mixed_target_0 | 243.27 | 12 | 8 | 58.05 | 237.30 | 243.27 | 217.84 |
| mixed_target_1 | 243.28 | 12 | 8 | 57.65 | 236.61 | 243.28 | 216.91 |

#### 1 MiB

| Scenario | Peak GB/s | Peak threads | 95% of peak at | 1 thread | 8 threads | 12 threads | 96 threads |
|---|---:|---:|---:|---:|---:|---:|---:|
| local_numa0 | 224.51 | 12 | 12 | 37.94 | 192.47 | 224.51 | 213.55 |
| local_numa1 | 225.33 | 12 | 12 | 38.08 | 191.81 | 225.33 | 213.33 |
| remote_0_to_1 | 118.84 | 12 | 8 | 21.90 | 114.50 | 118.84 | 110.21 |
| remote_1_to_0 | 117.33 | 12 | 8 | 22.05 | 114.14 | 117.33 | 109.65 |
| dual_local | 448.15 | 12 | 12 | 76.44 | 382.35 | 448.15 | 423.18 |
| dual_cross | 149.36 | 12 | 8 | 43.47 | 146.66 | 149.36 | 137.81 |
| mixed_target_0 | 236.51 | 12 | 8 | 59.68 | 227.22 | 236.51 | 216.92 |
| mixed_target_1 | 235.37 | 12 | 8 | 59.39 | 228.25 | 235.37 | 217.09 |

### Mixed Scenario Components

Mean per-process traffic across the three hosts:

| Size | Scenario | Local component at peak | Peak threads | Local component at 96 threads | Remote component at 96 threads |
|---|---|---:|---:|---:|---:|
| 4 KiB | mixed_target_0 | 168.04 | 12 | 158.84 | 59.00 |
| 4 KiB | mixed_target_1 | 167.97 | 12 | 158.03 | 58.88 |
| 1 MiB | mixed_target_0 | 158.94 | 12 | 158.18 | 58.74 |
| 1 MiB | mixed_target_1 | 158.69 | 16 | 158.64 | 58.45 |

This preserves the same interference pattern as the earlier fixed `96-thread` run: the local process loses about a quarter of its standalone local bandwidth while the remote contender stabilizes at roughly `58-59 GB/s`.

### uProf Summary

Mean `uProf total memory BW` across the three hosts for selected scenarios:

| Size | Scenario | 1 thread | 8 threads | 12 threads | 96 threads |
|---|---|---:|---:|---:|---:|
| 4 KiB | local_numa0 | 44.75 | 234.78 | 281.53 | 341.46 |
| 4 KiB | remote_0_to_1 | 28.23 | 144.47 | 152.88 | 181.99 |
| 4 KiB | dual_local | 83.77 | 438.41 | 520.92 | 518.93 |
| 4 KiB | mixed_target_0 | 65.86 | 277.60 | 283.20 | 295.88 |
| 1 MiB | local_numa0 | 47.65 | 238.60 | 280.73 | 345.25 |
| 1 MiB | remote_0_to_1 | 28.91 | 151.68 | 156.30 | 181.80 |
| 1 MiB | dual_local | 91.72 | 452.66 | 517.93 | 517.83 |
| 1 MiB | mixed_target_0 | 69.71 | 280.99 | 287.94 | 300.32 |

### Additional Observations From The Sweep

1. For both `4 KiB` and `1 MiB`, the single-NUMA local curve reaches `95%` of its peak at `12` threads, and the remote single-NUMA curve reaches `95%` of its peak already at `8` threads.
2. `1 MiB` changes only the ramp-up region. At low thread counts it is slightly more efficient, but once the workload reaches about `12-16` threads, both sizes converge to the same plateaus.
3. The saturated plateaus are effectively size-independent:
   - local single NUMA settles near `213-214 GB/s`
   - remote single NUMA settles near `109-110 GB/s`
   - dual local settles near `422-423 GB/s`
   - mixed target aggregate settles near `216-218 GB/s`
4. The interference pattern is also size-independent. In the mixed scenarios, the local component peaks near `159-168 GB/s` and the remote contributor stabilizes near `58-59 GB/s`.
5. Across the full sweep, the three hosts remain very close to each other. The synthetic no-YDB reproducer exposes the NUMA interference effect cleanly, but it does not recreate the host-to-host asymmetry that was seen with YDB.

### Additional Artifacts: uProf Per-Thread Graphs

Normalized `uProf total BW` by the total number of benchmark threads participating in the scenario:

- single-process scenarios: `uProf total BW / threads per process`
- two-process scenarios (`dual_*`, `mixed_*`): `uProf total BW / (2 * threads per process)`

Artifacts:

- `/home/robdrynkin/ydbwork/ydb/ydb/tools/memory_no_ydb_repro/reports/mem_bw_sweep_uprof_per_thread_4KiB.svg`
- `/home/robdrynkin/ydbwork/ydb/ydb/tools/memory_no_ydb_repro/reports/mem_bw_sweep_uprof_per_thread_1MiB.svg`
- `/home/robdrynkin/ydbwork/ydb/ydb/tools/memory_no_ydb_repro/render_sweep_svg.py`
