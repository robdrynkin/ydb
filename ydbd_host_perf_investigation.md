# YDBD Host Performance Investigation

Last updated: 2026-03-09

## Scope

Investigation target: why seemingly identical hosts run `ydbd` with noticeably different efficiency under comparable load.

Main host pairs investigated:

- `node-1302` vs `node-1307`
- `node-1304` vs `node-1308`
- `node-1306` vs `node-1308` (deepest comparison for `ydbd --node static`)

Primary symptom:

- same binary and broadly same workload
- worse hosts show higher CPU consumption across pools
- especially visible in IC pool and PDisk
- storage/static node improves when dynamic nodes are removed from the host

## High-level conclusions so far

The strongest current signal is not direct CPU-set overlap between static and dynamic pods, and not NIC IRQ affinity overlap. The dominant surviving explanation is excessive kernel scheduler / inter-CPU housekeeping activity on the CPUs used by the static `ydbd` on bad hosts. This correlates with:

- much higher `native_queued_spin_lock_slowpath` in kernel profiles
- much higher `nonvoluntary` context switches for the static `ydbd`
- much lower `IPC(sys)` and much higher `CPI(sys)`
- higher memory backend pressure and somewhat worse remote memory behavior

The issue is visible primarily when looking at the static node's CPUs and the static process, not in node-wide aggregate interrupt / context-switch totals.

## What was checked

- CPU architecture / frequency / turbostat counters
- NUMA placement of static and dynamic YDB nodes
- NUMA placement of NICs and storage
- cross-NUMA traffic and memory bandwidth
- scheduler statistics and wakeup behavior
- context-switch rates
- IRQ affinity
- `RPS` / `XPS` / `irqbalance`
- network driver and firmware inventory
- `perf` kernel symbol profiles
- process-level and system-level AMD uProf metrics
- `tcmalloc` visibility in profiles
- THP / hugepages
- memcpy / memory microbenchmarks
- unbound kernel workqueues
- `kernel.numa_balancing`

## Early findings: `1302` vs `1307`

Summary metrics collected for these nodes indicated that `1302` was more memory- and front-end-stressed than `1307`.

Notable differences:

- `ic_miss_rate_pct`: `28.39%` on `1302` vs `18.91%` on `1307`
- `l2_miss_rate_pct`: `34.36%` vs `30.87%`
- demand DRAM share: `21.85%` vs `15.37%`
- `mab_inflight_per_alloc`: `499.8` vs `207.8`
- L3 miss rate was close, but miss latency was slightly worse on `1302`

Interpretation at that stage:

- the bad host looked more memory-bound
- instruction-side behavior was also worse
- hardware prefetch looked less effective

The user also observed that when dynamic YDB pods were removed from `1302`, the storage/static node improved significantly. That became an important recurring clue.

## tcmalloc / perf visibility

Symbol profiles after moving dynamic load away from `1302` still showed some allocator cost on the worse host.

Representative profile fragments:

- `1302`: `_copy_to_iter` ~`15.4%`, `Spin` ~`12.5%`, `tcmalloc::Sampler::RecordAllocationSlow` ~`2.4%`
- `1307`: `_copy_to_iter` ~`16.8%`, `Spin` ~`11.5%`, allocator was less prominent

This suggested allocator activity might correlate with the bad hosts, but later deeper evidence pointed more strongly to scheduler / kernel-lock behavior than to allocator cost as the main cause.

## NUMA and memory findings

NUMA placement, cross-node traffic, and memory bandwidth were investigated repeatedly.

For the deeper `1306` vs `1308` comparison:

- total system memory bandwidth:
  - `1306`: `217.40 GB/s`
  - `1308`: `203.12 GB/s`
- remote memory bandwidth:
  - `1306`: `30.87 GB/s`
  - `1308`: `27.52 GB/s`
- remote share:
  - `1306`: `14.20%`
  - `1308`: `13.55%`

For the static node specifically:

- static `ydbd` memory bandwidth:
  - `1306`: `139.18 GB/s`
  - `1308`: `113.61 GB/s`
- static remote memory bandwidth:
  - `1306`: `13.18 GB/s`
  - `1308`: `10.39 GB/s`
- static remote share:
  - `1306`: `9.47%`
  - `1308`: `9.15%`

Interpretation:

- the bad host's static node is driving more memory traffic
- remote memory is somewhat worse on the bad host
- this matters, but it does not by itself explain the full performance gap

## CPU frequency and PSI

`turbostat` was used to compare `Bzy_MHz` and `Avg_MHz`, especially for `1304` vs `1308`.

Conclusions:

- raw core frequency did not explain the performance gap
- high `cpu PSI some` on the worse hosts pointed toward scheduler contention / time waiting for CPU
- there was no convincing sign that simple frequency throttling was the primary issue

## Deep comparison: static `ydbd` on `1306` vs `1308`

AMD uProf process metrics showed a key pattern:

- `Giga Instructions Per Sec` was almost the same:
  - `1306`: `87.58`
  - `1308`: `88.56`
- but `1306` was much less efficient:
  - `IPC (Sys + User)` lower by about `39%`
  - `IPC (Sys)` lower by about `44%`
  - `CPI (Sys + User)` higher by about `62%`
  - `CPI (Sys)` higher by about `84%`
  - `Backend_Bound` higher by about `22%`
  - `Backend_Bound.Memory` higher by about `25%`
  - `Remote DRAM Reads %` higher by about `57%`

Interpretation:

- the process retires roughly the same number of instructions per second
- but on the bad host it spends many more cycles stalled, waiting, or spinning
- therefore IPC collapses while GIPS stays close

## Kernel-path bottleneck

Kernel profiling exposed the biggest concrete difference.

On `1306`, `native_queued_spin_lock_slowpath` was very large:

- `ydbd.IC`: about `11.01%`
- `PDisk`: about `7.64%`
- `ydbd.System`: about `2.46%`
- combined: roughly `21%` of kernel cycles

On `1308`, the same slowpath was only about `1-2%` combined.

Interpretation:

- the bad host has major kernel lock contention
- this is fully consistent with lower `IPC(sys)`, higher `CPI(sys)`, and poor scaling under load

## Kernel IPC / CPI / misses

Kernel-only `perf stat` on the static process showed:

- `1306`:
  - `IPC(sys)` about `0.20`
  - `CPI(sys)` about `5.1`
- `1308`:
  - `IPC(sys)` about `0.36-0.37`
  - `CPI(sys)` about `2.75`

Cache and TLB behavior was also somewhat worse on `1306`, but not enough to explain the whole gap by itself.

## THP / hugepages

THP was checked explicitly.

Findings:

- THP mode was `madvise` on both hosts
- THP counters were effectively zero
- static `ydbd` had `AnonHugePages=0`

Conclusion:

- THP is not a meaningful factor in the current behavior

## Global node metrics vs process-local metrics

Node-wide aggregate metrics did not show the problem directly.

For `1306` vs `1308`:

- `context_switches/s`: `2.66M` vs `3.00M`
- `interrupts/s`: `1.83M` vs `1.92M`
- `softirq_total/s`: `1.05M` vs `1.14M`

So the worse node did not have higher totals.

But the static `ydbd` process did:

- total context switches:
  - `1306`: `1,319,950/s`
  - `1308`: `1,066,580/s`
- `nonvoluntary` context switches:
  - `1306`: `218,309/s`
  - `1308`: `2,013/s`

Interpretation:

- node-wide totals hide the issue
- the bad host is specifically preempting the static `ydbd` much more aggressively
- this is consistent with lock convoy and scheduler contention on the hot CPUs

## Isolation checks: static vs dynamic

This was checked carefully on `1306` and `1308`.

Confirmed:

- `1` static and `9` dynamic YDB processes
- CPU intersection between static and dynamic: `0`
- CCD intersection: none
- IRQ affinity crossing between static and dynamic CPUs: none

Additional checks:

- `RPS` disabled
- `XPS` hit only dynamic CPUs, not static CPUs
- `irqbalance` was not responsible

Conclusion:

- the problem is not caused by direct overlap of static and dynamic YDB CPU sets
- the problem is also not explained by NIC IRQ affinity crossing into the static CPUs

## Hidden kernel activity on static CPUs

The strongest remaining difference appeared when looking specifically at the CPUs used by the static `ydbd`.

Per-20-second windows on static CPUs:

- `1306`:
  - `SCHED softirq`: about `55k/s`
  - `IPI IWI`: about `222k/s`
  - `IPI RES`: about `35k/s`
  - `IPI TLB`: about `1.2k/s`
- `1308`:
  - `SCHED softirq`: about `8k/s`
  - `IPI IWI`: about `32k/s`
  - `IPI RES`: about `1.5k/s`
  - `IPI TLB`: about `0.6-0.7k/s`

Interpretation:

- bad hosts have much more scheduler / irq-work / reschedule activity on the static CPUs
- this is not primarily NET_RX traffic
- the issue looks like inter-CPU kernel housekeeping and coordination noise

`ksoftirqd` on static CPUs was nearly idle, so the problem is not softirq thread saturation.

## What these kernel events mean

- `SCHED softirq`: scheduler-related softirq work, including wakeups and runqueue-related housekeeping
- `IPI RES`: inter-processor interrupt asking another CPU to reschedule
- `IPI IWI`: interrupt used to deliver `irq_work` callbacks
- `IPI TLB`: TLB shootdown activity

These are kernel coordination signals, not user-space processes.

## Runtime experiments already performed

### 1. Moved unbound workqueues off YDB CPUs on `1306`

Housekeeping CPU list used:

- `0-7,40-71,96-103,192-199,232-263,288-294`

Reason:

- it is the complement of the configured `nohz_full` CPU set
- it avoids both static and dynamic YDB CPU ranges

Result:

- unbound workqueues were successfully moved away from static and dynamic YDB CPUs
- backup of previous workqueue masks:
  - `/var/tmp/wq-mask-2026-03-09-172350`
- but the main metrics barely moved:
  - `SCHED/RES/IWI` remained high
  - `IPC(sys)` / `CPI(sys)` did not materially improve

Conclusion:

- unbound workqueues were not the dominant source of the noise

### 2. Disabled `kernel.numa_balancing` at runtime on `1306`

Applied:

- `kernel.numa_balancing = 0`

Important:

- this was a runtime-only change
- no persistent sysctl file was written

Result:

- `IPI TLB` decreased somewhat
- but `SCHED softirq`, `IPI RES`, and `IPI IWI` remained very high
- `IPC(sys)` and `CPI(sys)` stayed effectively unchanged

Conclusion:

- automatic NUMA balancing was not the main cause

## Current host-side experimental state

As of the latest update:

- on `1306`, unbound workqueues are still moved to housekeeping CPUs
- on `1306`, `kernel.numa_balancing=0` is still active in runtime

This matters for future baselines and comparisons.

## BIOS / firmware / kernel comparison: `1306` vs `1308`

This comparison was done after the main runtime experiments above, so any runtime-only tuning already applied on `1306` must be kept in mind when reading current host state.

### BIOS / board / CPU microcode

The two hosts are effectively identical at the platform level:

- BIOS vendor: `American Megatrends International, LLC.`
- BIOS version: `M05`
- BIOS date: `11/21/2024`
- BIOS revision: `5.27`
- system product: `NB-D-SR-O2ASP5 0001`
- baseboard: `NB-E-MB-ASP5C5`, version `NB-E-MB-ASP5C5_3A`
- CPU model: `AMD EPYC 9654 96-Core Processor`
- socket/core/thread topology: identical
- CPU microcode: `0xa101148` on both

The only DMI-level differences were expected host identity fields such as serial number and UUID.

### Kernel build and boot config

Kernel and static config are also identical:

- kernel release: `6.14.8-5-nebius`
- build string: identical
- `/boot/config-$(uname -r)` SHA256:
  - `328359c7cf4e57423fc7839f80a312bdaaec04d8422263e26935a0a374688c4c`
  - same on both hosts
- key options matched:
  - `CONFIG_PREEMPT_DYNAMIC=y`
  - `CONFIG_PREEMPT_VOLUNTARY=y`
  - `CONFIG_NO_HZ_FULL=y`
  - `CONFIG_RCU_NOCB_CPU=y`
  - `CONFIG_NUMA_BALANCING=y`
  - `CONFIG_SCHED_CORE=y`
  - `CONFIG_HZ=1000`

Boot command line also matched, including:

- `transparent_hugepage=madvise`
- `amd_pstate=active`
- `nohz_full=8-39,200-231,72-95,264-287,104-191,295-383`
- `irqaffinity=40-71,232-263`

Interpretation:

- there is no BIOS-version mismatch
- there is no kernel-version mismatch
- there is no kernel-config mismatch
- there is no boot-cmdline mismatch between `1306` and `1308`

### Runtime kernel state

Current runtime differs only because of the experiments already performed:

- `1306`: `kernel.numa_balancing=0`
- `1308`: `kernel.numa_balancing=1`

This is an experimental difference introduced during the investigation, not an original host difference.

No boot-time `isolcpus` or `rcu_nocbs` configuration is present on either host.

### NIC driver / firmware

NIC stack is identical:

- main NIC: Mellanox `ConnectX-8`
- PF/VF PCI inventory count: same
- driver: `mlx5_core`
- driver version: `25.04-0.6.1`
- firmware: `40.46.3048 (MT_0000001167)`
- total PCI Ethernet functions: `66` on both hosts

Interpretation:

- the bad host is not explained by a different NIC firmware or mlx5 driver version

### NVMe / storage inventory

The broad PCIe storage topology matches:

- total PCI NVMe controllers: `18` on both hosts

Firmware revisions of the common devices also match:

- WD Ultrastar DC SN655: `RC610007`
- Micron 3500: `P8MA001`

But the detailed model mix is not exactly the same:

- `1306`:
  - `16` x `WUS5EA1A1ESP7E3` (`RC610007`)
  - `2` x `Micron_3500_MTFDKBA2T0TGD-1BK1AABYY` (`P8MA001`)
- `1308`:
  - `15` x `WUS5EA1A1ESP7E3` (`RC610007`)
  - `2` x `Micron_3500_MTFDKBA2T0TGD-1BK1AABYY` (`P8MA001`)
  - `1` x `SOLIDIGM SBFPF2BV0P12` (`5DV10602`)

Interpretation:

- platform, kernel, and NIC firmware are aligned
- the only concrete hardware inventory difference found so far is one NVMe device model in the small local-disk set
- this may matter only if that drive participates in the relevant workload, interrupt distribution, or system disk activity; it does not by itself explain the very large scheduler / lock differences already observed

## Current working hypothesis

The most likely explanation is:

- workload interaction with kernel scheduler / inter-CPU housekeeping causes much more `SCHED` softirq and `IPI RES/IWI` activity on the static CPUs of bad hosts
- this leads to heavy nonvoluntary preemption of the static `ydbd`
- preemption and cross-CPU coordination amplify kernel lock contention
- the result is high `native_queued_spin_lock_slowpath`, low `IPC(sys)`, and higher CPU cost across pools

This hypothesis fits:

- the improvement after removing dynamic pods from bad hosts
- the lack of direct CPU/IRQ overlap
- the large process-local but not node-wide context-switch difference
- the large kernel spinlock slowpath difference

## Things that do not currently look like the main cause

- THP / hugepages
- direct static/dynamic CPU overlap
- direct IRQ affinity overlap
- `RPS`
- `XPS` onto static CPUs
- unbound workqueues as the primary cause
- automatic NUMA balancing as the primary cause
- simple frequency throttling

## Most useful next steps

- identify producers of `IPI RES` / `IPI IWI` on bad hosts with scheduler and interrupt tracing
- evaluate boot-time isolation options:
  - `rcu_nocbs`
  - possibly `kthread_cpus`
  - possibly `isolcpus`
- run controlled A/B with static only, then add dynamic nodes one by one while measuring:
  - `nonvoluntary ctx`
  - `SCHED/IWI/RES`
  - `native_queued_spin_lock_slowpath`
  - `IPC(sys)` / `CPI(sys)`

## Targeted tracing of `IPI RES/IWI` sources

To avoid drowning in full-system trace volume, targeted tracing was run on the hottest static CPUs, selected by 1-second delta of `RES + IWI` from `/proc/interrupts`.

Selected CPU sets:

- `1306`: `344,147,350,149,179,152,337,108`
- `1308`: `336,317,338,121,120,343,148,319`

These 8-CPU subsets already showed a clear host difference:

- `1306` top per-CPU `RES+IWI` deltas: about `705-769/s`
- `1308` top per-CPU `RES+IWI` deltas: about `297-339/s`

Collected artifacts:

- `1306`
  - `/tmp/node1306.trace.20260309-180910.perf_sched.data`
  - `/tmp/node1306.trace.20260309-180910.perf_sched.timehist`
  - `/tmp/node1306.trace.20260309-180910.ipi_sched.data`
  - `/tmp/node1306.trace.20260309-180910.ipi_sched.script`
  - `/tmp/node1306.events_ftrace.$(date +%Y%m%d-%H%M%S).log`
- `1308`
  - `/tmp/node1308.trace.20260309-180909.perf_sched.data`
  - `/tmp/node1308.trace.20260309-180909.perf_sched.timehist`
  - `/tmp/node1308.trace.20260309-180909.ipi_sched.data`
  - `/tmp/node1308.trace.20260309-180909.ipi_sched.script`
  - `/tmp/node1308.events_ftrace.$(date +%Y%m%d-%H%M%S).log`

### Important implementation note

Direct tracing of `irq_work_*` functions through `ftrace` function filter or `kprobe_events` was attempted but did not work cleanly on this kernel build:

- `irq_work_queue`, `irq_work_queue_on`, `irq_work_run`, `irq_work_single` were not accepted by `set_ftrace_filter`
- direct `kprobe_events` insertion on `irq_work_queue` failed with `Invalid argument`

Because of that, `irq_work` attribution was taken from the `ipi:ipi_send_cpu` callsite itself. This still gives exact and useful attribution, because the tracepoint exposes:

- current task (`comm/pid`)
- destination CPU
- `callsite`
- `callback`

### `perf` findings from `ipi_send_cpu`

Dominant callsites on both hosts:

- `ttwu_queue_wakelist -> generic_smp_call_function_single_interrupt`
- `irq_work_queue -> nohz_full_kick_func`
- `irq_work_queue_on -> nohz_full_kick_func`
- `irq_work_queue_on -> generic_smp_call_function_single_interrupt`
- `wakeup_preempt -> 0x0`

But the bad host had much more `irq_work` / wakeup-preempt activity.

`1306` (`perf`, 2-second window on hot CPUs):

- `ttwu_queue_wakelist -> generic_smp_call_function_single_interrupt`: `85145`
- `irq_work_queue -> nohz_full_kick_func`: `8813`
- `irq_work_queue_on+0x6f -> nohz_full_kick_func`: `1593`
- `irq_work_queue_on+0x4b -> generic_smp_call_function_single_interrupt`: `1041`
- `wakeup_preempt`: `767`

`1308` (`perf`, same method):

- `ttwu_queue_wakelist -> generic_smp_call_function_single_interrupt`: `88195`
- `irq_work_queue -> nohz_full_kick_func`: `2055`
- `irq_work_queue_on+0x6f -> nohz_full_kick_func`: `1452`
- `irq_work_queue_on+0x4b -> generic_smp_call_function_single_interrupt`: `212`
- `wakeup_preempt`: `128`

Interpretation:

- ordinary cross-CPU wakeups (`ttwu_queue_wakelist`) dominate on both hosts
- but the bad host has a much larger extra component from:
  - `irq_work_queue`
  - `irq_work_queue_on`
  - `wakeup_preempt`
- this matches the earlier observation of inflated `IWI/RES` on `1306`
- `nohz_full_kick_func` is a particularly strong signal that part of the extra noise is tied to NOHZ/housekeeping kicks

### Which threads are sending these IPIs

On `1306`, main senders seen in `perf ipi_send_cpu` were:

- `PdGetEv`
- `ydbd.System`
- `PDisk`
- `ydbd.IC`
- `PdSbmEv`

On `1308`, the same classes dominate, but with a different balance and smaller `irq_work`-related component.

This is important: the extra cross-CPU activity is not coming from some random background task. It is being generated by the storage/system/IC-related YDB threads themselves plus their close kernel interaction.

### Independent confirmation from `ftrace` event tracing

Even without direct `irq_work_*` function tracing, `ftrace` event logs confirmed the same ranking of `ipi_send_cpu` callsites.

`1306` (`ftrace`, 1-second window):

- `ttwu_queue_wakelist -> generic_smp_call_function_single_interrupt`: `18951`
- `irq_work_queue -> nohz_full_kick_func`: `742`
- `irq_work_queue_on+0x6f -> nohz_full_kick_func`: `322`
- `irq_work_queue_on+0x4b -> generic_smp_call_function_single_interrupt`: `87`
- `wakeup_preempt`: `70`

`1308` (`ftrace`, same method):

- `ttwu_queue_wakelist -> generic_smp_call_function_single_interrupt`: `11176`
- `irq_work_queue -> nohz_full_kick_func`: `265`
- `irq_work_queue_on+0x6f -> nohz_full_kick_func`: `222`
- `irq_work_queue_on+0x4b -> generic_smp_call_function_single_interrupt`: `23`
- `wakeup_preempt`: `11`

Interpretation:

- the `ftrace` and `perf` pictures agree
- `1306` has materially more `irq_work_queue` and `wakeup_preempt` traffic on the hot static CPUs

### `softirq_raise` on the traced hot CPUs

`ftrace` counts:

- `1306`
  - `NET_RX`: `624`
  - `SCHED`: `240`
  - `TIMER`: `86`
  - `RCU`: `59`
- `1308`
  - `NET_RX`: `261`
  - `SCHED`: `116`
  - `TIMER`: `68`
  - `RCU`: `16`

Interpretation:

- even on the reduced 8-CPU subset, `1306` shows higher `SCHED` and `RCU` softirq activity
- the excess is not limited to wakeup IPIs only

### Bottom line from tracing

The targeted traces strengthen the working hypothesis:

- both hosts do lots of ordinary cross-CPU wakeups
- the bad host has an additional, significantly larger component of:
  - `irq_work_queue`
  - `irq_work_queue_on`
  - `nohz_full_kick_func`
  - `wakeup_preempt`
- those signals line up directly with the observed excess `IWI/RES`, nonvoluntary preemption, and kernel spinlock slowpath on `1306`

## Re-compare after load restart: `1306` vs `1308` on 2026-03-10

After the user restarted the load, the same short metric set was collected again over a fresh 12-second window.

### Important caveat

`1306` was still in the experimentally modified runtime state:

- `kernel.numa_balancing=0`
- unbound workqueues still moved to housekeeping CPUs

`1308` remained in the default runtime state for `kernel.numa_balancing`.

Even with that caveat, the new result is important because the old asymmetry did **not** reproduce in the same direction.

### Fresh process-local comparison

Static `ydbd` context switches per second:

- `1306`
  - total: `1.115M/s`
  - voluntary: `927k/s`
  - nonvoluntary: `187k/s`
- `1308`
  - total: `1.096M/s`
  - voluntary: `804k/s`
  - nonvoluntary: `292k/s`

Kernel-only `perf stat` on static `ydbd`:

- `1306`
  - `IPC(sys)`: `0.215`
  - `CPI(sys)`: `4.65`
  - `cache-miss%`: `24.72%`
- `1308`
  - `IPC(sys)`: `0.191`
  - `CPI(sys)`: `5.25`
  - `cache-miss%`: `23.89%`

Interpretation:

- in this fresh window, `1308` is no longer better than `1306`
- it is slightly worse in `IPC(sys)` / `CPI(sys)`
- it is also worse in `nonvoluntary` preemption

### Fresh static-CPU interrupt / softirq comparison

Per-second sums over the static CPU set:

- `1306`
  - `IWI`: `188,980/s`
  - `RES`: `34,895/s`
  - `TLB`: `1,280/s`
  - `SCHED softirq`: `45,641/s`
  - `NET_RX softirq`: `13,192/s`
  - `RCU softirq`: `7,146/s`
- `1308`
  - `IWI`: `210,307/s`
  - `RES`: `40,294/s`
  - `TLB`: `917/s`
  - `SCHED softirq`: `45,113/s`
  - `NET_RX softirq`: `7,129/s`
  - `RCU softirq`: `7,271/s`

Interpretation:

- the earlier large gap in `IWI/RES/SCHED` did not persist in the same form
- in this window, `1308` actually had higher `IWI` and `RES`
- `SCHED softirq` became nearly equal

### Fresh kernel symbol snapshot

Short `cycles:k` snapshots on both hosts showed very similar structure:

- `_copy_to_iter` dominant on both
- `schedule/__schedule` around `11%`
- `try_to_wake_up` around `6-7%`
- `native_queued_spin_lock_slowpath` present but only around `~1-1.6%` in the visible stack fragments

This is very different from the earlier state where `1306` had roughly `~21%` combined kernel cycles in `native_queued_spin_lock_slowpath`.

### Meaning of the re-compare

This new result is strategically important:

- the old pattern "`1306` bad, `1308` good" is not stable
- after load restart, the two hosts are much closer
- in this particular window, `1308` is slightly worse in the key scheduler-related metrics

This weakens the hypothesis that the problem is a fixed BIOS / firmware / hardware defect of `1306`.

It strengthens a different interpretation:

- the dominant factor is likely dynamic runtime state:
  - what exactly the workload is doing at the moment
  - which hot shards / actors / queues landed where
  - current cross-CPU wakeup pattern
  - current PDisk / completion behavior
  - possibly pod / interrupt / kernel wakeup phase alignment

In other words: the phenomenon appears at least partly **state-dependent**, not purely **host-identity-dependent**.

## Static-node flamegraphs collected on 2026-03-10

Perf recording command used on both hosts:

```bash
sudo perf record -a -g -F 250 -p $PID --proc-map-timeout=10000 -- sleep 30
```

Remote artifacts:

- `1306`
  - `/tmp/node1306.static.flame.perf.data`
  - `/tmp/node1306.static.flame.svg`
- `1308`
  - `/tmp/node1308.static.flame.perf.data`
  - `/tmp/node1308.static.flame.svg`

Local copies:

- `/tmp/node1306.static.flame.svg`
- `/tmp/node1308.static.flame.svg`
- `/tmp/node1302.static.flame.svg`

`FlameGraph` was copied to the hosts as `/tmp/FlameGraph.tgz`, unpacked to `/tmp/FlameGraph`, and SVG generation was done on-host to avoid transferring the full `perf script` output over the network.

Additional matching artifact collected later:

- `1302`
  - remote: `/tmp/node1302.static.flame.perf.data`, `/tmp/node1302.static.flame.svg`
  - local: `/tmp/node1302.static.flame.svg`

## XXH3_64bits_update comparison: `perf` vs `bpftrace`

Code inspection in the repo showed an important detail:

- `ydb/library/actors/interconnect/interconnect_tcp_session.h` and `packet.h` define `XXH_INLINE_ALL`
- therefore `XXH3_64bits_update()` is compiled as a local inline-copy, not as a normal exported function symbol
- in the running `ydbd` binary the relevant local symbol is:
  - `_ZL29XXH_INLINE_XXH3_64bits_updateP25XXH_NAMESPACEXXH3_state_sPKvm`
  - same address on all checked hosts: `0x18e31330`

This matters for observability:

- normal global-symbol probing on `XXH3_64bits_update` does not work
- probing must target the local inline copy instead

### Perf-side view

Since direct `perf` lifecycle on these hosts was unreliable in this session, the `perf` side for XXH was taken from the already collected flamegraph SVGs.

This is a **sampling-based on-stack share**, not exact per-call latency.

Aggregated `XXH_INLINE_XXH3_64bits_update` share in the saved SVGs:

- `1302`
  - frames found: `2`
  - summed share: `2.69%`
- `1306`
  - frames found: `3`
  - summed share: `4.07%`
- `1308`
  - frames found: `3`
  - summed share: `6.08%`

Interpretation:

- XXH update is visibly more prominent in the sampled CPU profile on `1306` and especially `1308` than on `1302`
- this by itself does not prove the function is slower there; it may also be called more often or process more bytes

### Bpftrace-side view

Two `bpftrace` passes were used on each host:

1. all-call entry-only pass for `calls/s` and `avg len`
2. sampled entry/return pass (roughly `1/1024` of calls) for latency

Important caveat:

- on such a hot microfunction, `bpftrace` entry/return probes add non-trivial overhead
- therefore absolute latency numbers are inflated
- the useful part is the **relative comparison between hosts**, especially with comparable `avg len`

#### All-call count / length, 3-second windows

- `1302`
  - `calls`: `2,963,192`
  - `calls/s`: `987,731`
  - `avg len`: `2691 B`
  - `bytes/s`: `2.658 GB/s`
- `1306`
  - `calls`: `2,849,814`
  - `calls/s`: `949,938`
  - `avg len`: `2674 B`
  - `bytes/s`: `2.541 GB/s`
- `1308`
  - `calls`: `2,813,535`
  - `calls/s`: `937,845`
  - `avg len`: `2695 B`
  - `bytes/s`: `2.528 GB/s`

Interpretation:

- call rate and average buffer length are very close across the three hosts
- `1302` is only modestly higher in calls and total bytes
- therefore large differences in sampled CPU share are unlikely to be explained only by traffic volume

#### Sampled latency, 5-second windows

Overall sampled latency:

- `1302`
  - sampled calls: `4631`
  - `avg latency`: `36,778 ns`
  - sampled `avg len`: `2718 B`
  - sampled `ns/byte`: `13.53`
- `1306`
  - sampled calls: `4339`
  - `avg latency`: `43,993 ns`
  - sampled `avg len`: `2566 B`
  - sampled `ns/byte`: `17.14`
- `1308`
  - sampled calls: `4132`
  - `avg latency`: `44,695 ns`
  - sampled `avg len`: `2686 B`
  - sampled `ns/byte`: `16.64`

Latency by length bucket:

- `<=64 B`
  - `1302`: `35,545 ns`
  - `1306`: `43,981 ns`
  - `1308`: `44,711 ns`
- `65..256 B`
  - `1302`: `35,531 ns`
  - `1306`: `43,748 ns`
  - `1308`: `46,332 ns`
- `257..1024 B`
  - `1302`: `36,268 ns`
  - `1306`: `42,709 ns`
  - `1308`: `44,506 ns`
- `1025..4096 B`
  - `1302`: `36,181 ns`
  - `1306`: `45,026 ns`
  - `1308`: `45,363 ns`
- `>4096 B`
  - `1302`: `38,651 ns`
  - `1306`: `43,479 ns`
  - `1308`: `44,120 ns`

Interpretation:

- in the current measurement window, `1302` is consistently better than both `1306` and `1308` for this XXH path
- `1306` and `1308` are very close; `1308` is slightly worse in per-call latency buckets, while `1306` is slightly worse in sampled `ns/byte`
- because call rate and `avg len` are similar, the perf-side difference is not just “more hashing traffic”; it is consistent with genuinely higher cost per XXH update on `1306/1308`

### Relation to earlier findings

This XXH result fits the broader investigation in a few ways:

- it supports the general picture that “same amount of work” can cost different amounts of CPU on different hosts / runs
- it is compatible with the earlier memory / backend-pressure findings: XXH is a memory-heavy streaming routine, so worse backend behavior can make it more expensive
- it also matches the later conclusion that the phenomenon is state-dependent: the “cheap” or “expensive” XXH behavior is not necessarily tied to one fixed host identity

It does **not** by itself prove that XXH is the root cause.

More likely interpretation:

- XXH is acting as a good probe of the current memory / scheduling state
- when the host/run is in a worse state, even a relatively straightforward streaming routine becomes more expensive
- that is consistent with the previously seen drops in `IPC(sys)`, increases in `CPI(sys)`, and episodes of higher scheduler / lock pressure

## Maintenance note

This file is intended to be the rolling investigation journal for the host performance issue. Before further experiments, re-read it and append new findings instead of reconstructing context from chat history alone.

## Standalone XXH3 update benchmark

A dedicated standalone benchmark was added to the repo:

- source: `/home/robdrynkin/ydbwork/ydb/ydb/tools/xxh3_update_bench/main.cpp`
- target: `ydb/tools/xxh3_update_bench`
- binary symlink after build:
  - `/home/robdrynkin/ydbwork/ydb/ydb/tools/xxh3_update_bench/xxh3_update_bench`

Purpose:

- measure `XXH3_64bits_update()` outside `ydbd`
- keep the exact inline build model (`XXH_INLINE_ALL`)
- separate “hot buffer” and “streaming / larger working set” modes
- optionally pin benchmark threads to specific CPUs

Key options:

- `--size`: bytes per `XXH3_64bits_update()` call
- `--working-set-bytes`: total per-thread cycled data set
  - `== size` approximates hot-buffer mode
  - much larger values approximate streaming / LLC / DRAM-sensitive mode
- `--iterations`
- `--warmup-iterations`
- `--threads`
- `--cpu-base`
- `--cpu-stride`
- `--reset-each-iter`

Sanity run on the local build host with `size=2686`, `iterations=100000`, `threads=1`:

- hot-buffer style (`working-set-bytes = size`)
  - `~16.14 GB/s`
  - `~166 ns/call`
  - `~0.124 cycles/byte`
- larger working set (`working-set-bytes = size * 8192`, about `22 MB`)
  - `~15.40 GB/s`
  - `~174 ns/call`
  - `~0.130 cycles/byte`

Interpretation:

- even in this isolated microbenchmark, enlarging the working set already makes the same XXH update path slower
- this supports the earlier hypothesis that the real variability in `ydbd` can be driven by locality / memory-state differences, not only by the pure instruction stream of the function

## Standalone XXH3 benchmark matrix on `1302`, `1306`, `1308`

Date: `2026-03-10`

Raw results file:

- `/tmp/xxh3_matrix_results.jsonl`

CPU selection policy:

- selected free CPUs not overlapping current `ydbd` cpusets
- excluded `cpu0` and `cpu1`
- ranked candidates by short-window (`1s`) local CPU busy time, interrupts, softirq, `NET_RX`, `SCHED`, `RCU`
- took the two lowest-score CPUs on each NUMA node

Chosen CPUs:

- `1302`
  - node `0`: `263`, `195`
  - node `1`: `334`, `356`
- `1306`
  - node `0`: `192`, `3`
  - node `1`: `143`, `161`
- `1308`
  - node `0`: `193`, `192`
  - node `1`: `189`, `188`

Benchmark modes:

- `small_hot`
  - `size=22`
  - `working_set_bytes=22`
- `avg_hot`
  - `size=2686`
  - `working_set_bytes=2686`
- `avg_stream_local`
  - `size=2686`
  - `working_set_bytes=512 MiB`
  - memory bound to the same NUMA node as the CPU
- `avg_stream_remote`
  - `size=2686`
  - `working_set_bytes=512 MiB`
  - memory bound to the opposite NUMA node

Threading:

- `threads=1`
- CPU pinning done inside the benchmark (`pthread_setaffinity_np`)
- memory binding done via `numactl --membind=<node>`

### Aggregate host-level results

Mean over the four tested CPUs per host:

- `1302`
  - `small_hot`: `4.322 GB/s`, `0.5587 cycles/byte`
  - `avg_hot`: `23.586 GB/s`, `0.1018 cycles/byte`
  - `avg_stream_local`: `19.295 GB/s`, `0.1228 cycles/byte`
  - `avg_stream_remote`: `16.957 GB/s`, `0.1401 cycles/byte`
- `1306`
  - `small_hot`: `4.531 GB/s`, `0.5291 cycles/byte`
  - `avg_hot`: `24.289 GB/s`, `0.0987 cycles/byte`
  - `avg_stream_local`: `18.804 GB/s`, `0.1266 cycles/byte`
  - `avg_stream_remote`: `15.713 GB/s`, `0.1517 cycles/byte`
- `1308`
  - `small_hot`: `4.446 GB/s`, `0.5395 cycles/byte`
  - `avg_hot`: `24.099 GB/s`, `0.0995 cycles/byte`
  - `avg_stream_local`: `15.480 GB/s`, `0.1617 cycles/byte`
  - `avg_stream_remote`: `11.788 GB/s`, `0.2010 cycles/byte`

### NUMA-local asymmetry

Mean `avg_stream_local` throughput by NUMA node:

- `1302`
  - node `0`: `19.453 GB/s`
  - node `1`: `19.136 GB/s`
  - node `1` vs node `0`: `-1.6%`
- `1306`
  - node `0`: `19.893 GB/s`
  - node `1`: `17.715 GB/s`
  - node `1` vs node `0`: `-10.9%`
- `1308`
  - node `0`: `19.021 GB/s`
  - node `1`: `11.938 GB/s`
  - node `1` vs node `0`: `-37.2%`

Per-core `avg_stream_local` values:

- `1302`
  - node `0`: `18.675`, `20.232`
  - node `1`: `18.953`, `19.319`
- `1306`
  - node `0`: `19.720`, `20.065`
  - node `1`: `18.968`, `16.463`
- `1308`
  - node `0`: `18.519`, `19.524`
  - node `1`: `12.213`, `11.664`

Interpretation:

- `1302` is well balanced between NUMA nodes
- `1306` has a noticeable but moderate node-1 weakness
- `1308` has a very large node-1 weakness for the streaming working set
- on `1308`, node-1 local performance is close to node-0 remote performance, which is a very strong signal

### Remote NUMA penalty

Average penalty of `avg_stream_remote` vs `avg_stream_local`:

- `1302`
  - throughput delta: `-12.1%`
  - cycles/byte delta: `+14.2%`
- `1306`
  - throughput delta: `-16.5%`
  - cycles/byte delta: `+19.8%`
- `1308`
  - throughput delta: `-19.9%`
  - cycles/byte delta: `+30.7%`

### Interpretation of the standalone matrix

This matrix is important because it separates instruction-stream effects from locality effects:

- `small_hot` is similar on all three hosts
- `avg_hot` is also very close on all three hosts
- the strong divergence appears only when the working set becomes large enough to stress the memory hierarchy

This means:

- the basic compute part of `XXH3_64bits_update()` is not meaningfully different between hosts
- the inter-host difference is primarily about locality / memory subsystem behavior
- on `1308`, the issue is not just host-wide; it is strongly concentrated in one NUMA node

This gives a concrete mechanism that can coexist with the earlier “bad host changes from run to run” observation:

- if a bad run happens to place more hot threads / queues / data on the weaker NUMA node, the host looks bad
- if a later run places the hot path differently, the same host can look good again

### Compact NUMA tables

`avg_stream_*` throughput by CPU NUMA node and memory NUMA node:

| Host | CPU NUMA | Mem NUMA | Local/Remote | Mean GB/s | Samples |
|---|---:|---:|---|---:|---:|
| `1302` | `0` | `0` | local | `19.453` | `2` |
| `1302` | `0` | `1` | remote | `17.702` | `2` |
| `1302` | `1` | `0` | remote | `16.213` | `2` |
| `1302` | `1` | `1` | local | `19.136` | `2` |
| `1306` | `0` | `0` | local | `19.893` | `2` |
| `1306` | `0` | `1` | remote | `16.918` | `2` |
| `1306` | `1` | `0` | remote | `14.507` | `2` |
| `1306` | `1` | `1` | local | `17.715` | `2` |
| `1308` | `0` | `0` | local | `19.021` | `2` |
| `1308` | `0` | `1` | remote | `12.007` | `2` |
| `1308` | `1` | `0` | remote | `11.568` | `2` |
| `1308` | `1` | `1` | local | `11.938` | `2` |

The same numbers grouped more compactly:

| Host | NUMA 0 local | NUMA 0 remote | NUMA 1 local | NUMA 1 remote |
|---|---:|---:|---:|---:|
| `1302` | `19.453` | `17.702` | `19.136` | `16.213` |
| `1306` | `19.893` | `16.918` | `17.715` | `14.507` |
| `1308` | `19.021` | `12.007` | `11.938` | `11.568` |

Current `ydbd` placement by NUMA node at the time of this snapshot:

| Host | Static on NUMA 0 | Static on NUMA 1 | Dynamic on NUMA 0 | Dynamic on NUMA 1 | Total |
|---|---:|---:|---:|---:|---:|
| `1302` | `0` | `1` | `7` | `1` | `9` |
| `1306` | `0` | `1` | `7` | `1` | `9` |
| `1308` | `0` | `1` | `7` | `1` | `9` |

Notes:

- static was identified by `--node static`
- dynamic nodes were identified by the presence of `--tenant ...`
- current live state on these three hosts is `1 static + 8 dynamic`, not `1 static + 9 dynamic`
- the single dynamic on NUMA `1` coexists with the static node on NUMA `1` on all three hosts

## Standalone `memset + memcpy` benchmark matrix

To separate pure memory-system behavior from any hash/state effects, a second standalone benchmark was added:

- tool: `ydb/tools/memcpy_memset_bench`
- single-thread only
- each iteration does:
  - `memset(memsetDst + offset, value, size)`
  - `memcpy(memcpyDst + offset, src + offset, size)`
- memory traffic per iteration is therefore `3 * size`
- CPU pinning is done inside the benchmark
- NUMA locality is controlled externally with `numactl --membind=<node>`

The same CPU matrix was used as for the `xxh3` benchmark:

- `1302`
  - NUMA `0`: CPUs `263`, `195`
  - NUMA `1`: CPUs `334`, `356`
- `1306`
  - NUMA `0`: CPUs `192`, `3`
  - NUMA `1`: CPUs `143`, `161`
- `1308`
  - NUMA `0`: CPUs `193`, `192`
  - NUMA `1`: CPUs `189`, `188`

Modes:

- `small_hot`: `size=22`, `working_set=22`
- `avg_hot`: `size=2686`, `working_set=2686`
- `avg_stream_local`: `size=2686`, `working_set=512 MiB`, local memory
- `avg_stream_remote`: `size=2686`, `working_set=512 MiB`, remote memory

### Host-level means

Using memory traffic throughput (`memset write + memcpy read + memcpy write`) as the primary metric:

- `1302`
  - `small_hot`: `12.671 GB/s`
  - `avg_hot`: `132.003 GB/s`
  - `avg_stream_local`: `19.766 GB/s`
  - `avg_stream_remote`: `11.914 GB/s`
- `1306`
  - `small_hot`: `12.916 GB/s`
  - `avg_hot`: `137.025 GB/s`
  - `avg_stream_local`: `20.013 GB/s`
  - `avg_stream_remote`: `11.911 GB/s`
- `1308`
  - `small_hot`: `12.824 GB/s`
  - `avg_hot`: `132.593 GB/s`
  - `avg_stream_local`: `16.205 GB/s`
  - `avg_stream_remote`: `8.323 GB/s`

### NUMA matrix

`avg_stream_*` traffic throughput by CPU NUMA node and memory NUMA node:

| Host | NUMA 0 local | NUMA 0 remote | NUMA 1 local | NUMA 1 remote |
|---|---:|---:|---:|---:|
| `1302` | `20.832` | `11.946` | `18.699` | `11.882` |
| `1306` | `21.110` | `11.934` | `18.915` | `11.889` |
| `1308` | `20.299` | `9.198` | `12.112` | `7.448` |

### Remote penalty

Average `avg_stream_remote` vs `avg_stream_local` traffic throughput penalty:

- `1302`: `-39.7%`
- `1306`: `-40.5%`
- `1308`: `-48.6%`

### Interpretation

This is a very important cross-check against the `xxh3` benchmark:

- `small_hot` and `avg_hot` are again very close on all three hosts
- the large difference appears only with the large working set
- `1308` again shows a strong NUMA-`1` weakness

This means the bad behavior is not specific to:

- `XXH3_64bits_update()`
- checksum state handling
- branch pattern inside the hash

Instead, the asymmetry survives in a much simpler test that is essentially just:

- streaming writes
- streaming reads
- NUMA-local vs NUMA-remote memory access

So the current evidence points even more strongly to a memory-locality / NUMA-path problem, not to a hash-function-specific problem.

## Current system-wide NUMA bandwidth snapshot

Current snapshot taken on `2026-03-10` under the then-active workload.

### `AMDuProfPcm` system-wide memory bandwidth

This was available on `1306` and `1308` directly via:

```bash
sudo AMDuProfPcm --msr -m memory -a -A system -d 5 -C
```

Results:

| Host | Total BW | Local Read | Local Write | Remote Read | Remote Write | Remote Total | Remote Share |
|---|---:|---:|---:|---:|---:|---:|---:|
| `1306` | `241.85` | `139.59` | `67.47` | `33.63` | `1.16` | `34.79` | `14.4%` |
| `1308` | `252.25` | `147.67` | `71.93` | `32.27` | `0.39` | `32.66` | `12.9%` |

Observations:

- `1308` currently has higher total system memory bandwidth than `1306`
- `1308` also has slightly lower remote-share than `1306`
- most of the remote component is in reads, not writes

### `perf` fallback for near/far DRAM fills

For all three hosts, a common fallback was taken via:

```bash
sudo perf stat -a -e ls_any_fills_from_sys.dram_io_near,ls_any_fills_from_sys.dram_io_far -- sleep 5
```

The counts were converted assuming `64 B` per DRAM fill.
This is not identical to the `uProf` total memory-bandwidth metric, but it is a useful common read-side locality snapshot across all three hosts.

| Host | Near DRAM fill BW | Far DRAM fill BW | Total | Far Share |
|---|---:|---:|---:|---:|
| `1302` | `37.30` | `6.70` | `44.00` | `15.2%` |
| `1306` | `36.63` | `8.74` | `45.38` | `19.3%` |
| `1308` | `55.90` | `8.05` | `63.95` | `12.6%` |

Observations:

- in this current window `1308` has the strongest near-memory fill rate
- `1306` has the highest far/remote share
- `1302` sits between them on near bandwidth, but closer to `1308` than to `1306` on remote share

Methodological note:

- `uProf` on `1302` was not directly usable in-place because the host lacked the required processor-model configuration files for the copied binary
- the `perf` near/far fill numbers are therefore the only current common-method snapshot across `1302`, `1306`, and `1308`

### Update: `uProf` installed on `1302`

Later, the full `uProf` installation tree was copied from `1306` to `1302`:

- source layout on good hosts: `/opt/AMDuProf_5.2-606`
- installed on `1302` to the same path
- symlink created:
  - `/usr/local/bin/AMDuProfPcm -> /opt/AMDuProf_5.2-606/bin/AMDuProfPcm`

`msr` had to be loaded explicitly on `1302`:

```bash
sudo modprobe msr
```

After that, the same system-wide memory command worked on all three hosts:

```bash
sudo AMDuProfPcm --msr -m memory -a -A system -d 5 -C
```

### Uniform current system-wide memory snapshot

Current apples-to-apples `uProf` memory snapshot for all three hosts:

| Host | Total BW | Local Read | Local Write | Remote Read | Remote Write | Remote Total | Remote Share |
|---|---:|---:|---:|---:|---:|---:|---:|
| `1302` | `242.60` | `142.21` | `69.33` | `29.91` | `1.14` | `31.05` | `12.8%` |
| `1306` | `254.82` | `148.30` | `71.13` | `34.17` | `1.21` | `35.38` | `13.9%` |
| `1308` | `249.07` | `145.54` | `70.45` | `32.64` | `0.44` | `33.08` | `13.3%` |

Interpretation of this current snapshot:

- all three hosts are now in the same measurement method
- `1306` currently has the highest total system memory bandwidth
- `1306` also has the highest remote traffic in absolute `GB/s`
- `1302` currently has the lowest remote share
- the differences are real but not extreme; they are much smaller than the single-NUMA-node asymmetry seen in the standalone microbenchmarks

Saved raw snapshots:

- `/tmp/node1302.system_memory.uprof.json`
- `/tmp/node1306.system_memory.uprof.json`
- `/tmp/node1308.system_memory.uprof.json`
- `/tmp/system_memory_uprof_all.json`

### NUMA-1-only current memory snapshot

The same `uProf` memory collection was then repeated with scope restricted to `NUMA 1`:

```bash
sudo AMDuProfPcm --msr -m memory -c numa=1 -d 5 -C
```

Results:

| Host | NUMA1 Total BW | Local Read | Local Write | Remote Read | Remote Write | Remote Total | Remote Share |
|---|---:|---:|---:|---:|---:|---:|---:|
| `1302` | `125.19` | `83.24` | `30.81` | `11.10` | `0.03` | `11.13` | `8.9%` |
| `1306` | `132.60` | `86.52` | `33.66` | `12.37` | `0.04` | `12.41` | `9.4%` |
| `1308` | `139.41` | `91.18` | `34.75` | `13.42` | `0.06` | `13.48` | `9.7%` |

Interpretation:

- in this current system-wide `NUMA 1` snapshot, `1308` does **not** look weak
- in fact, `1308` currently has the highest `NUMA 1` total bandwidth of the three
- this is important because it shows the standalone `NUMA 1` microbenchmark weakness on `1308` is not trivially visible as a system-wide `NUMA 1` bandwidth collapse under the current live workload

Saved raw snapshots:

- `/tmp/node1302.numa1_memory.uprof.json`
- `/tmp/node1306.numa1_memory.uprof.json`
- `/tmp/node1308.numa1_memory.uprof.json`
- `/tmp/numa1_memory_uprof_all.json`

## `XXH3_64bits_update` cache-threshold sweep on `NUMA 1`

To understand at which cache level the degradation starts, `xxh3_update_bench` was run with:

- `size = 2 KiB`
- `working-set-bytes` in:
  - `2 KiB`
  - `16 KiB`
  - `64 KiB`
  - `512 KiB`
  - `2 MiB`
  - `16 MiB`
  - `64 MiB`
- CPUs pinned on `NUMA 1`
- memory bound either to:
  - `NUMA 1` (`local`)
  - `NUMA 0` (`remote`)

Two CPUs per host were used and averaged:

- `1302`: `334`, `356`
- `1306`: `143`, `161`
- `1308`: `189`, `188`

Iterations:

- payload per run: about `16 GiB`
- `iterations = 8,388,608`
- `warmup_iterations = 100,000`

### Throughput (`GB/s`)

Local (`NUMA 1` CPU, `NUMA 1` memory):

| Working set | `1302` | `1306` | `1308` |
|---|---:|---:|---:|
| `2 KiB` | `24.567` | `24.571` | `24.719` |
| `16 KiB` | `24.353` | `24.794` | `25.014` |
| `64 KiB` | `24.297` | `24.730` | `24.917` |
| `512 KiB` | `24.060` | `24.637` | `24.752` |
| `2 MiB` | `23.907` | `24.156` | `24.396` |
| `16 MiB` | `23.791` | `24.114` | `24.046` |
| `64 MiB` | `20.900` | `20.733` | `14.607` |

Remote (`NUMA 1` CPU, `NUMA 0` memory):

| Working set | `1302` | `1306` | `1308` |
|---|---:|---:|---:|
| `2 KiB` | `24.672` | `24.702` | `24.927` |
| `16 KiB` | `24.225` | `24.796` | `24.518` |
| `64 KiB` | `24.196` | `24.710` | `24.785` |
| `512 KiB` | `24.146` | `24.557` | `24.616` |
| `2 MiB` | `23.912` | `24.057` | `24.595` |
| `16 MiB` | `24.019` | `24.041` | `23.856` |
| `64 MiB` | `19.434` | `19.331` | `14.290` |

### Efficiency (`cycles/byte`)

Local:

| Working set | `1302` | `1306` | `1308` |
|---|---:|---:|---:|
| `2 KiB` | `0.0975` | `0.0975` | `0.0969` |
| `16 KiB` | `0.0984` | `0.0966` | `0.0958` |
| `64 KiB` | `0.0986` | `0.0969` | `0.0962` |
| `512 KiB` | `0.0996` | `0.0973` | `0.0968` |
| `2 MiB` | `0.1002` | `0.0992` | `0.0982` |
| `16 MiB` | `0.1007` | `0.0993` | `0.0996` |
| `64 MiB` | `0.1141` | `0.1150` | `0.1635` |

Remote:

| Working set | `1302` | `1306` | `1308` |
|---|---:|---:|---:|
| `2 KiB` | `0.0971` | `0.0970` | `0.0961` |
| `16 KiB` | `0.0989` | `0.0966` | `0.0978` |
| `64 KiB` | `0.0990` | `0.0970` | `0.0967` |
| `512 KiB` | `0.0992` | `0.0976` | `0.0973` |
| `2 MiB` | `0.1002` | `0.0996` | `0.0974` |
| `16 MiB` | `0.0997` | `0.0997` | `0.1005` |
| `64 MiB` | `0.1227` | `0.1234` | `0.1766` |

### Interpretation

This sweep gives a much cleaner cache-level picture:

- up to `16 MiB`, all three hosts are very close on `NUMA 1`
- there is no meaningful degradation at:
  - `64 KiB` (already beyond `L1d = 32 KiB`)
  - `2 MiB` (already beyond `L2 = 1 MiB`)
- there is also no strong divergence at `16 MiB`, which still fits inside one `32 MiB` `L3`
- the major divergence appears only at `64 MiB`, i.e. when the working set is clearly beyond one `L3` slice

This strongly suggests:

- the problem does **not** start at the `L1` or `L2` boundary
- the first clear cliff is after the `L3` working-set threshold is crossed
- on `1308 NUMA 1`, the weak behavior is therefore much more consistent with last-level-cache / DRAM / memory-path behavior than with small-cache effects

Saved raw results:

- `/tmp/xxh3_ws_numa1_results.jsonl`

## Static thread-pool runtime locality by CPU / CCD

Date: `2026-03-10`

Goal:

- inspect where the static node's hot pools (`IC`, `System`, `PDisk`) are actually running now
- compare how local or spread they are across the static node's allowed CCDs

Method:

- static `ydbd` PID found via `pgrep -fa 'ydbd.*--node static'`
- thread names taken from `/proc/$pid/task/*/comm`
- current CPU (`psr`) sampled `50` times with `100 ms` spacing
- each sampled CPU mapped to `CCD` through `/sys/devices/system/cpu/cpu*/cache/index3/id`
- pools classified as:
  - `System`: `ydbd.System`
  - `IC`: `ydbd.IC`
  - `PDisk`: `PDisk`, `PdGetEv`, `PdSbmEv`, `PdTrim`, `PdCmpl_0`

Important interpretation note:

- all three static processes have the same allowed CPU set:
  - `104-135,144-159,168-183,296-327,336-351,360-375`
- this allowed set spans `8` CCDs on `NUMA 1`:
  - `17,18,19,20,21,22,23,24`
- therefore pool-level `distinct_ccd=8` by itself is expected
- the useful signal is per-thread locality:
  - how much time a thread spends on its top CCD
  - how many distinct CCDs a thread visits during the sample window

Thread counts seen in the static node:

- `1302`
  - `System`: `48`
  - `IC`: `48`
  - `PDisk`: `320`
- `1306`
  - `System`: `48`
  - `IC`: `48`
  - `PDisk`: `320`
- `1308`
  - `System`: `48`
  - `IC`: `48`
  - `PDisk`: `304`

### Per-thread locality summary

Mean over threads in each pool:

| Host | Pool | avg top-CCD share | avg distinct CCDs | avg distinct CPUs |
|---|---|---:|---:|---:|
| `1302` | `System` | `0.267` | `7.25` | `38.38` |
| `1302` | `IC` | `0.250` | `7.65` | `38.52` |
| `1302` | `PDisk` | `0.364` | `6.72` | `33.19` |
| `1306` | `System` | `0.281` | `7.27` | `38.02` |
| `1306` | `IC` | `0.231` | `7.90` | `39.50` |
| `1306` | `PDisk` | `0.358` | `6.69` | `33.31` |
| `1308` | `System` | `0.219` | `7.92` | `40.25` |
| `1308` | `IC` | `0.207` | `7.98` | `41.65` |
| `1308` | `PDisk` | `0.330` | `6.89` | `34.89` |

Additional compact locality indicators:

| Host | Pool | threads with top CCD >= 50% | threads with <= 2 CCDs |
|---|---|---:|---:|
| `1302` | `System` | `8.3%` | `6.2%` |
| `1302` | `IC` | `2.1%` | `0.0%` |
| `1302` | `PDisk` | `20.0%` | `17.2%` |
| `1306` | `System` | `10.4%` | `4.2%` |
| `1306` | `IC` | `0.0%` | `0.0%` |
| `1306` | `PDisk` | `18.1%` | `15.9%` |
| `1308` | `System` | `0.0%` | `0.0%` |
| `1308` | `IC` | `0.0%` | `0.0%` |
| `1308` | `PDisk` | `17.1%` | `12.5%` |

### What this means

There is a real runtime-locality difference between the hosts:

- `System` and `IC` are the most spread pools on all hosts
- `1308` is the most diffuse of the three, especially for `IC` and `System`
  - lower average top-CCD share
  - higher average distinct-CCD count
  - higher average distinct-CPU count
- `PDisk` is more local than `IC`/`System` on all hosts, but even there `1308` is not more local than `1302/1306`

The strongest practical difference is:

- on `1302` / `1306`, `System` still has a small subset of threads that remain noticeably more local
- on `1308`, neither `System` nor `IC` has such a subset in this sample window; they are almost completely spread across all `8` static CCDs

### Interpretation relative to the earlier memory findings

This does **not** prove that wide migration is the root cause.

But it is consistent with the current memory-centered hypothesis:

- once the hot path becomes sensitive to post-`L3` locality, a pool that keeps bouncing across many CPUs / CCDs has fewer chances to preserve useful LLC locality
- `1308` already looked weakest on `NUMA 1` in the standalone large-working-set benchmarks
- at runtime, `1308` also shows the least local `IC/System` placement among the three checked hosts

So the current combined picture is:

- the standalone cliff appears only after leaving one `L3`
- and on the live process, the `IC` / `System` work on `1308` is also the least CCD-local

Saved raw snapshots:

- `/tmp/node1302.static_pool_locality.json`
- `/tmp/node1306.static_pool_locality.json`
- `/tmp/node1308.static_pool_locality.json`

## Manual static-thread pinning on `1307`

Date: `2026-03-10`

Context:

- static node was redeployed with:
  - `IC`: `32` threads
  - `System`: `32` threads
- static process still has the same `128` allowed CPUs across `8` CCDs on `NUMA 1`

Observed thread counts on `1307` static `ydbd`:

- `System`: `32`
- `IC`: `32`
- `PDisk`: `320`
- `Other`: `261`

Applied manual pinning plan:

- `IC` -> `CCD 20 + 24`
  - CPUs: `104-119,296-311`
  - each of the `32` `IC` threads pinned to its own single CPU
- `System` -> `CCD 19 + 22`
  - CPUs: `128-135,144-151,320-327,336-343`
  - each of the `32` `System` threads pinned to its own single CPU
- `PDisk` -> `CCD 17 + 21 + 23`
  - CPUs: `152-159,168-183,344-351,360-375`
  - all `PDisk/Pd*` threads pinned to this shared 3-CCD range
- `Other` -> `CCD 18`
  - CPUs: `120-127,312-319`
  - all remaining threads pinned to this shared 1-CCD range

Backup of previous affinities:

- `/tmp/node1307.static_affinity_backup.2109807.json`

Verification after applying:

- `IC`
  - exactly `32` threads
  - exactly one thread per CPU on:
    - `104-119,296-311`
- `System`
  - exactly `32` threads
  - exactly one thread per CPU on:
    - `128-135,144-151,320-327,336-343`
- `PDisk`
  - `320` threads
  - all pinned to:
    - `152-159,168-183,344-351,360-375`
- `Other`
  - `261` threads
  - all pinned to:
    - `120-127,312-319`

This host is now in an explicitly partitioned static-thread layout and should be treated as a special runtime state for subsequent comparisons.

## Manual static-thread pinning on `1308`

Date: `2026-03-10`

Context:

- static node currently runs with:
  - `IC`: `32` threads
  - `System`: `32` threads
- static process has the same `128` allowed CPUs across `8` CCDs on `NUMA 1`

Observed thread counts on `1308` static `ydbd`:

- `System`: `32`
- `IC`: `32`
- `PDisk`: `304`
- `Other`: `261`

Applied manual pinning plan (same as on `1307`):

- `IC` -> `CCD 20 + 24`
  - CPUs: `104-119,296-311`
  - each of the `32` `IC` threads pinned to its own single CPU
- `System` -> `CCD 19 + 22`
  - CPUs: `128-135,144-151,320-327,336-343`
  - each of the `32` `System` threads pinned to its own single CPU
- `PDisk` -> `CCD 17 + 21 + 23`
  - CPUs: `152-159,168-183,344-351,360-375`
  - all `PDisk/Pd*` threads pinned to this shared 3-CCD range
- `Other` -> `CCD 18`
  - CPUs: `120-127,312-319`
  - all remaining threads pinned to this shared 1-CCD range

Backup of previous affinities:

- `/tmp/node1308.static_affinity_backup.1731589.json`

Verification after applying:

- `IC`
  - exactly `32` threads
  - exactly one thread per CPU on:
    - `104-119,296-311`
- `System`
  - exactly `32` threads
  - exactly one thread per CPU on:
    - `128-135,144-151,320-327,336-343`
- `PDisk`
  - `304` threads
  - all pinned to:
    - `152-159,168-183,344-351,360-375`
- `Other`
  - `261` threads
  - all pinned to:
    - `120-127,312-319`

This host is also now in an explicitly partitioned static-thread layout and must be treated as a special runtime state for further comparisons.

## Fleet-wide static-thread pinning on `1302-1309`

Date: `2026-03-10`

After validating the layout on `1307` and `1308`, the same partitioning scheme was applied to the static `ydbd` on:

- `1302`
- `1303`
- `1304`
- `1305`
- `1306`
- `1307`
- `1308`
- `1309`

Common target layout on every host:

- `IC`
  - one thread per CPU on:
    - `104-119,296-311`
- `System`
  - one thread per CPU on:
    - `128-135,144-151,320-327,336-343`
- `PDisk`
  - all `PDisk/Pd*` threads on:
    - `152-159,168-183,344-351,360-375`
- `Other`
  - all remaining threads on:
    - `120-127,312-319`

Verified final state:

- `1302`
  - `IC`: `32` one-CPU pins
  - `System`: `32` one-CPU pins
  - `PDisk`: `320` threads on 3-CCD range
  - `Other`: `261` threads on 1-CCD range
- `1303`
  - `IC`: `32` one-CPU pins
  - `System`: `32` one-CPU pins
  - `PDisk`: `320` threads on 3-CCD range
  - `Other`: `261` threads on 1-CCD range
- `1304`
  - `IC`: `32` one-CPU pins
  - `System`: `32` one-CPU pins
  - `PDisk`: `320` threads on 3-CCD range
  - `Other`: `261` threads on 1-CCD range
- `1305`
  - `IC`: `32` one-CPU pins
  - `System`: `32` one-CPU pins
  - `PDisk`: `320` threads on 3-CCD range
  - `Other`: `261` threads on 1-CCD range
- `1306`
  - `IC`: `32` one-CPU pins
  - `System`: `32` one-CPU pins
  - `PDisk`: `320` threads on 3-CCD range
  - `Other`: `261` threads on 1-CCD range
- `1307`
  - `IC`: `32` one-CPU pins
  - `System`: `32` one-CPU pins
  - `PDisk`: `320` threads on 3-CCD range
  - `Other`: `261` threads on 1-CCD range
- `1308`
  - `IC`: `32` one-CPU pins
  - `System`: `32` one-CPU pins
  - `PDisk`: `304` threads on 3-CCD range
  - `Other`: `261` threads on 1-CCD range
- `1309`
  - `IC`: `32` one-CPU pins
  - `System`: `32` one-CPU pins
  - `PDisk`: `320` threads on 3-CCD range
  - `Other`: `261` threads on 1-CCD range

At this point the static nodes on `1302-1309` are in a common manually partitioned runtime state, suitable for controlled follow-up comparison.

## Current YDB workload profile snapshot for non-YDB reproduction

A dedicated workload-profile snapshot was collected on `2026-03-11` across `node-1302 .. node-1309` to describe the current host-level load generated by YDB in a form suitable for synthetic reproduction without running YDB itself.

Artifacts:

- human-readable summary: `/home/robdrynkin/ydbwork/ydb/ydb_workload_repro_profile.md`
- raw machine-readable bundle: `/home/robdrynkin/ydbwork/ydb/ydb_workload_repro_profile.json`

The snapshot captures, per host:

- static-process CPU load, syscall rate, RSS, `/proc/<pid>/io` rates
- host network throughput / packet rate
- host NVMe throughput / IOPS
- scheduler / IPI / softirq noise on the static CPU set
- current thread-pool sizes and CPU layout visible from the static process

This snapshot should be used as the baseline envelope when constructing a minimal non-YDB reproducer.

## 2026-03-12 UProf IOD snapshot: `1302` vs `1308` with YDB enabled

Goal: compare package-level IOD / Data Fabric traffic on `node-1302` and `node-1308` while YDB is running again, using a single synchronized UProf cumulative window per host.

Command used on each host:

```bash
sudo AMDuProfPcm -r >/dev/null 2>&1 || true
sudo AMDuProfPcm --msr -m memory,ccm_bw,xgmi,dma,pcie -a -A package -C -d 5 -o /tmp/u_all_<host>_5s.csv
```

Static YDB runtime context during the snapshot:

- `1302`: static pid `1234431`, `Cpus_allowed_list=104-135,144-159,168-183,296-327,336-351,360-375`, `nonvoluntary_ctxt_switches=871`
- `1308`: static pid `2588095`, `Cpus_allowed_list=104-135,144-159,168-183,296-327,336-351,360-375`, `nonvoluntary_ctxt_switches=2164`

Artifacts:

- `/tmp/u_all_1302_5s.csv`
- `/tmp/u_all_1308_5s.csv`

Key package-level metrics:

- Package 1 is the socket where the static CPU set lives.

```text
Metric                                 1302 pkg1   1308 pkg1   delta
Total Mem Bw (GB/s)                    130.30      146.83      +12.7%
Local DRAM Read Data Bytes (GB/s)       87.74       97.31      +10.9%
Local DRAM Write Data Bytes (GB/s)      31.13       35.53      +14.1%
Remote DRAM Read Data Bytes (GB/s)      11.38       13.90      +22.1%
Remote DRAM Write Data Bytes (GB/s)      0.05        0.08      +60.0% (tiny absolute)
Remote DRAM share                         8.8%        9.5%      +0.7 pp
Local Inbound Read Data Bytes (GB/s)    83.73       91.88      +9.7%
Local Outbound Write Data Bytes (GB/s)  27.68       31.83      +15.0%
Remote Inbound Read Data Bytes (GB/s)   19.15       20.62      +7.7%
Remote Outbound Write Data Bytes (GB/s)  1.09        0.48      -56.0%
xGMI Outbound Data Bytes (GB/s)         14.34       14.41      ~same
Total PCIe Bandwidth (GB/s)              7.74        7.32      -5.4%
Total Upstream DMA RW (GB/s)             7.69        7.28      -5.3%
```

Interesting observations:

1. The main difference is on package 1: `1308` is currently carrying materially more memory traffic than `1302` on the socket where static YDB runs.
2. The increase is broad and uniform rather than a hotspot:
   - all 12 memory channels on package 1 are higher on `1308` by roughly `~0.8-1.1 GB/s` read bandwidth per channel;
   - per-channel spread is still very low on both hosts, so there is no obvious single-channel imbalance.
3. `xGMI` is effectively identical on package 1 (`14.34` vs `14.41 GB/s`), so the current host difference is not explained by a spike in socket-to-socket traffic.
4. `PCIe` and `DMA` on package 1 are also very close; they do not explain the package-1 delta.
5. The delta is therefore concentrated in the CPU<->DRAM / CPU<->CCM path, not in PCIe or xGMI.
6. Package 0 is also similar between hosts; the most noticeable package-0 difference is that `1308` has higher `Remote Inbound Read Data Bytes` (`4.60` vs `1.70 GB/s`), but package 0 is not where the static CPU set is pinned.

Current interpretation:

- In this window, `1308` is simply doing more work on the static socket than `1302` in terms of DRAM and CPU-facing fabric traffic.
- There is no sign here of:
  - a bad memory-channel imbalance,
  - a bad PCIe quad imbalance,
  - or a pathological xGMI spike unique to one host.
- The interesting part is not a fabric routing anomaly, but that the package hosting static YDB on `1308` is currently consuming more local and remote memory bandwidth almost across the board.

## 2026-03-12 free-chiplet local NUMA1 memory test: `1302` vs `1308`

Goal: compare raw local memory access speed from a free `NUMA1` CCD while YDB is running.

Setup:

- hosts: `1302`, `1308`
- chosen free CCD on both hosts: `CCD26`
- CPUs used: `136-143` (first SMT half of that CCD)
- memory policy: `numactl --membind=1`
- benchmark: `/tmp/mem_bw_stress`
- benchmark params: `--threads 8 --working-set-bytes 67108864 --warmup-sec 1 --duration-sec 5`
- sizes tested: `4096`, `1048576`
- simultaneous UProf snapshot: `AMDuProfPcm --msr -m memory -c numa=1 -C -d 6`

Artifacts:

- `/tmp/mem_ccd26_compare/1302_4096.out`
- `/tmp/mem_ccd26_compare/1302_4096.csv`
- `/tmp/mem_ccd26_compare/1302_1048576.out`
- `/tmp/mem_ccd26_compare/1302_1048576.csv`
- `/tmp/mem_ccd26_compare/1308_4096.out`
- `/tmp/mem_ccd26_compare/1308_4096.csv`
- `/tmp/mem_ccd26_compare/1308_1048576.out`
- `/tmp/mem_ccd26_compare/1308_1048576.csv`

Results:

```text
size=4KiB, threads=8, CPU=136-143, membind=NUMA1
host   bench payload  bench traffic  cycles/traffic-byte  uProf NUMA1 total  remote share
1302   7.97 GB/s      23.92 GB/s     0.801                174.87 GB/s        6.5%
1308   6.30 GB/s      18.89 GB/s     1.014                172.43 GB/s        7.0%

delta  1308 vs 1302:  -21.0%         -21.0%               +26.6%              -1.4%            +0.5 pp
```

```text
size=1MiB, threads=8, CPU=136-143, membind=NUMA1
host   bench payload  bench traffic  cycles/traffic-byte  uProf NUMA1 total  remote share
1302   9.50 GB/s      28.51 GB/s     0.672                171.28 GB/s        6.1%
1308   6.24 GB/s      18.73 GB/s     1.023                169.34 GB/s        7.1%

delta  1308 vs 1302:  -34.3%         -34.3%               +52.2%              -1.1%            +0.9 pp
```

Interesting observation:

- On the free `CCD26`, local NUMA1 benchmark throughput is clearly lower on `1308` than on `1302`.
- The slowdown is visible for both sizes and is stronger for `1MiB` than for `4KiB`.
- At the same time, total `NUMA1` memory bandwidth seen by UProf is almost the same on the two hosts.

Interpretation:

- This is not a simple “NUMA1 memory bus is slower on 1308” result.
- Instead, while the overall NUMA1 memory subsystem is carrying roughly the same total bandwidth, the isolated workload on free `CCD26` gets less useful throughput on `1308`.
- That points to a locality / contention / scheduling interaction affecting that CCD or its path to memory under live YDB load, rather than a gross socket-wide memory-bandwidth collapse.

## 2026-03-12 free-chiplet local NUMA1 memory test: `1305` vs `1302`

Same setup as `1308 vs 1302`:

- free `NUMA1` `CCD26`
- CPUs `136-143`
- `numactl --membind=1`
- `/tmp/mem_bw_stress --threads 8 --working-set-bytes 67108864 --warmup-sec 1 --duration-sec 5`
- sizes: `4096`, `1048576`
- simultaneous `AMDuProfPcm --msr -m memory -c numa=1 -C -d 6`

Artifacts:

- `/tmp/mem_ccd26_compare_1305/1302_4096.out`
- `/tmp/mem_ccd26_compare_1305/1302_4096.csv`
- `/tmp/mem_ccd26_compare_1305/1302_1048576.out`
- `/tmp/mem_ccd26_compare_1305/1302_1048576.csv`
- `/tmp/mem_ccd26_compare_1305/1305_4096.out`
- `/tmp/mem_ccd26_compare_1305/1305_4096.csv`
- `/tmp/mem_ccd26_compare_1305/1305_1048576.out`
- `/tmp/mem_ccd26_compare_1305/1305_1048576.csv`

Results:

```text
size=4KiB
host   bench payload  bench traffic  cycles/traffic-byte  uProf NUMA1 total  remote share
1302   7.97 GB/s      23.92 GB/s     0.801                174.87 GB/s        6.5%
1305   6.35 GB/s      19.05 GB/s     1.005                168.53 GB/s        6.8%

delta  1305 vs 1302:  -20.4%         -20.4%               +25.5%              -3.6%            +0.3 pp
```

```text
size=1MiB
host   bench payload  bench traffic  cycles/traffic-byte  uProf NUMA1 total  remote share
1302   9.50 GB/s      28.51 GB/s     0.672                171.28 GB/s        6.1%
1305   6.34 GB/s      19.01 GB/s     1.008                170.42 GB/s        6.7%

delta  1305 vs 1302:  -33.3%         -33.3%               +50.0%              -0.5%            +0.5 pp
```

Interpretation:

- `1305` reproduces the same pattern seen on `1308`: the isolated local benchmark on a free `NUMA1` CCD is materially slower than on `1302`.
- The slowdown is visible even though `uProf` reports nearly identical total `NUMA1` memory bandwidth on the two hosts.
- This again points away from a simple socket-wide memory-bandwidth deficit and toward a more local execution-context / CCD-path effect under live YDB load.
