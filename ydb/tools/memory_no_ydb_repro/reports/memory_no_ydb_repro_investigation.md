
## Local NUMA: interleave CCD vs dense packing

Отдельно проверили, чем именно объясняется knee у `local_numa*` кривых.

Новый compare-sweep:
- report dir: `/home/robdrynkin/ydbwork/ydb/ydb/tools/memory_no_ydb_repro/reports/local_packing_compare`
- сценарии: только `local_numa0` и `local_numa1`
- два режима:
  - `interleave`: потоки разносятся по CCD round-robin
  - `dense`: потоки плотно заполняют CPU подряд внутри NUMA

Результат:
- knee действительно определяется CCD-locality, а не просто числом потоков.
- При `dense` throughput растет ступенями по `8` потоков, что соответствует заполнению одного CCD (`8` физических ядер).
- При `interleave` первые `12` потоков сразу расходятся по `12` CCD сокета, поэтому рост выглядит почти линейным до `12`.

Representative bench traffic averages:
- `4KiB`:
  - `8 threads`: `interleave ~199 GB/s`, `dense ~41.5 GB/s`
  - `12 threads`: `interleave ~235 GB/s`, `dense ~79.9 GB/s`
  - `24 threads`: `interleave ~224 GB/s`, `dense ~110.0 GB/s`
  - `48 threads`: `interleave ~216 GB/s`, `dense ~177.1 GB/s`
  - `96 threads`: `interleave ~213.4 GB/s`, `dense ~213.3 GB/s`
- `1MiB`:
  - `8 threads`: `interleave ~190.8 GB/s`, `dense ~41.4 GB/s`
  - `12 threads`: `interleave ~223.9 GB/s`, `dense ~79.3 GB/s`
  - `24 threads`: `interleave ~218.9 GB/s`, `dense ~109.6 GB/s`
  - `48 threads`: `interleave ~214.5 GB/s`, `dense ~176.8 GB/s`
  - `96 threads`: `interleave ~213.5 GB/s`, `dense ~213.5 GB/s`

Вывод:
- Один densely packed CCD дает примерно `41 GB/s`.
- Полная NUMA полоса (`~213-214 GB/s`) появляется только когда задействованы все CCD сокета.
- Значит исходный knee около `12` потоков в baseline действительно был артефактом выбранного CPU-order (`interleave_ccd`), а не фундаментальным свойством "12 потоков сами по себе".
