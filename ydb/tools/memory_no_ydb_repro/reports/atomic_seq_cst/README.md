# atomic_seq_cst

Вариант synthetic memory benchmark, где перед каждым `memcpy` выполняется
общий для всех потоков `fetch_add(1, std::memory_order_seq_cst)`.

Источник бенчмарка:
- `/home/robdrynkin/ydbwork/ydb/ydb/tools/memory_no_ydb_repro/mem_bw_stress/main.cpp`

Ключевой флаг запуска:
- `--shared-seq-cst-atomic-before-memcpy`

Полный sweep сейчас выполняется тем же набором сценариев, что и baseline:
- hosts: `1302`, `1306`, `1308`
- sizes: `4KiB`, `1MiB`
- threads per process: `1,2,4,8,12,16,24,32,48,64,96`
- scenarios: `local_*`, `remote_*`, `dual_*`, `mixed_target_*`

По завершении в этой папке появятся:
- `memory_no_ydb_sweep.json`
- `mem_bw_sweep_bench_*.svg`
- `mem_bw_sweep_uprof_*.svg`
- `mem_bw_sweep_uprof_per_thread_*.svg`
- `mem_bw_sweep_components_mixed_target_*.svg`
