# YDB Workload Reproduction Profile

Snapshot date: 2026-03-11
Window: 5 seconds per host
Hosts: node-1302 .. node-1309
Target process: `ydbd --node static` on each host

## What This File Captures

This is the current steady-state workload profile created by YDB on the investigated hosts. The intent is to reproduce an equivalent host-level load without YDB itself.

Metrics included:

- static process CPU consumption
- static process memory footprint
- total syscall rate for the static process
- process read/write syscall-derived throughput from `/proc/<pid>/io`
- host network throughput and packet rate
- host NVMe throughput and IOPS
- scheduler / IPI / softirq activity on the static CPU set
- current thread counts and CPU layout visible from the static process

## Current Static Process Layout

- static process allowed CPU list on all hosts: `104-135,144-159,168-183,296-327,336-351,360-375`
- this is 128 logical CPUs on NUMA 1 / 8 CCDs
- current pool sizes from runtime sampling:
  - node-1302.ydb-dev.ik8s.nebiuscloud.net: IC=48, System=48, PDisk=320, Other=261, total=677
  - node-1303.ydb-dev.ik8s.nebiuscloud.net: IC=48, System=48, PDisk=320, Other=261, total=677
  - node-1304.ydb-dev.ik8s.nebiuscloud.net: IC=48, System=48, PDisk=320, Other=261, total=677
  - node-1305.ydb-dev.ik8s.nebiuscloud.net: IC=48, System=48, PDisk=320, Other=261, total=677
  - node-1306.ydb-dev.ik8s.nebiuscloud.net: IC=48, System=48, PDisk=320, Other=261, total=677
  - node-1307.ydb-dev.ik8s.nebiuscloud.net: IC=48, System=48, PDisk=320, Other=261, total=677
  - node-1308.ydb-dev.ik8s.nebiuscloud.net: IC=48, System=48, PDisk=304, Other=261, total=661
  - node-1309.ydb-dev.ik8s.nebiuscloud.net: IC=48, System=48, PDisk=320, Other=261, total=677

## Summary Ranges For Synthetic Reproduction

- Static process CPU load: 85.96-124.21 cores (median 95.03 cores)
- Static process user CPU: 48.47-69.16 cores (median 54.22 cores)
- Static process system CPU: 37.49-55.05 cores (median 40.96 cores)
- Static process RSS: 17.48-18.28 GiB (median 18.02 GiB)
- Total static-process syscall rate: 2.843-4.006 M/s (median 3.865 M/s)
- Static-process read syscalls (`syscr`): 28359.0-178951.0 /s (median 144731.0 /s)
- Static-process write syscalls (`syscw`): 487.0-679.0 /s (median 552.0 /s)
- Static-process `rchar` throughput: 15.32-16.21 GB/s (median 15.8 GB/s)
- Static-process `write_bytes` throughput: 15.71-16.67 GB/s (median 16.25 GB/s)
- Host RX throughput: 13.58-14.32 GB/s (median 13.93 GB/s)
- Host TX throughput: 13.32-14.55 GB/s (median 13.96 GB/s)
- Host RX packet rate: 1.66-1.81 Mpps (median 1.76 Mpps)
- Host TX packet rate: 1.65-1.81 Mpps (median 1.77 Mpps)
- Host NVMe read throughput: 0.0-13.3 MB/s (median 12.2 MB/s)
- Host NVMe write throughput: 15680.4-16567.6 MB/s (median 16188.4 MB/s)
- Host NVMe read IOPS: 3.0-179.0 IOPS (median 114.0 IOPS)
- Host NVMe write IOPS: 233058.0-256877.0 IOPS (median 249991.0 IOPS)
- Static CPU IPI IWI rate: 98481.0-228172.0 /s (median 142845.0 /s)
- Static CPU IPI RES rate: 11046.0-39515.0 /s (median 18987.0 /s)
- Static CPU IPI TLB rate: 372.0-782.0 /s (median 435.0 /s)
- Static CPU SCHED softirq rate: 25690.0-52325.0 /s (median 34107.0 /s)
- Static CPU NET_RX softirq rate: 7183.0-22156.0 /s (median 18736.0 /s)
- Static CPU RCU softirq rate: 4349.0-10854.0 /s (median 7385.0 /s)

## Per-Host Snapshot

| Host | CPU cores | User | Sys | RSS GiB | Syscalls M/s | rchar GB/s | write_bytes GB/s | RX GB/s | TX GB/s | Write MB/s | Write IOPS | IWI/s | RES/s | SCHED/s |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| node-1302 | 91.4 | 51.9 | 39.5 | 18.18 | 3.944 | 15.79 | 16.23 | 13.95 | 13.67 | 16179.4 | 250479 | 130330 | 15984 | 33089 |
| node-1303 | 98.7 | 56.5 | 42.2 | 18.10 | 3.785 | 15.82 | 16.26 | 13.92 | 14.03 | 16197.5 | 249503 | 159321 | 21990 | 39983 |
| node-1304 | 110.1 | 62.8 | 47.3 | 17.48 | 3.538 | 15.90 | 16.34 | 13.93 | 14.31 | 16247.5 | 246364 | 195012 | 28734 | 36787 |
| node-1305 | 124.2 | 69.2 | 55.0 | 18.28 | 2.843 | 15.32 | 15.71 | 13.58 | 13.32 | 15680.4 | 233058 | 228172 | 39515 | 52325 |
| node-1306 | 91.3 | 51.6 | 39.8 | 18.07 | 3.994 | 16.21 | 16.67 | 14.32 | 13.98 | 16567.6 | 256877 | 121450 | 14703 | 30749 |
| node-1307 | 100.0 | 57.1 | 42.8 | 17.65 | 3.569 | 15.65 | 16.07 | 13.73 | 14.55 | 15997.1 | 244905 | 155360 | 22815 | 35125 |
| node-1308 | 86.0 | 48.5 | 37.5 | 17.97 | 4.006 | 15.82 | 16.27 | 14.01 | 13.94 | 16223.3 | 252007 | 98481 | 11046 | 25690 |
| node-1309 | 88.3 | 49.5 | 38.9 | 17.93 | 3.974 | 15.73 | 16.17 | 13.91 | 13.42 | 16133.5 | 255387 | 112956 | 13161 | 28783 |

## Representative Memory Pressure

Exact current AMD uProf system memory bandwidth was available on the hosts where `AMDuProfPcm` is installed. Recent representative measurements from this investigation were:

| Host | Total BW GB/s | Local Read | Local Write | Remote Read | Remote Write | Remote Share |
|---|---:|---:|---:|---:|---:|---:|
| node-1302 | 242.60 | 142.21 | 69.33 | 29.91 | 1.14 | 12.8% |
| node-1306 | 254.82 | 148.30 | 71.13 | 34.17 | 1.21 | 13.9% |
| node-1308 | 249.07 | 145.54 | 70.45 | 32.64 | 0.44 | 13.3% |

These numbers are included because the previous experiments showed that the observed failures are tied to memory activity after the workload leaves the 32 MiB L3 domain, not to pure compute throughput.

## Practical Reproduction Target

A minimal non-YDB reproducer should aim to match the following envelope on one host:

- one main process bound to the static CPU set (`104-135,144-159,168-183,296-327,336-351,360-375`)
- about 86-124 busy logical cores inside that process, with roughly 55-60% user and 40-45% system CPU inside the process
- about 18 GiB resident memory for the main process
- about 2.8-4.0 million syscalls per second from the main process
- about 15-16 GB/s process-side read/write stream through the hot path
- about 13.6-14.6 GB/s RX/TX host network throughput in each direction, ~1.7-1.8 Mpps in each direction
- about 15.7-16.6 GB/s host NVMe writes, roughly 233k-257k write IOPS
- static-CPU scheduler/inter-CPU noise in the range listed above (`IWI`, `RES`, `SCHED`, `NET_RX`, `RCU`)
- hot working sets must exceed a single 32 MiB L3 domain; earlier experiments showed the interesting degradation starts only after leaving L3 and going to DRAM, even for local NUMA memory

Raw machine-readable bundle: `/home/robdrynkin/ydbwork/ydb/ydb_workload_repro_profile.json`
