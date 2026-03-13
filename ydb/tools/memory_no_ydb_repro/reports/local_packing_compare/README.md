# local_packing_compare

Сравнение только local NUMA сценариев:
- `local_numa0`
- `local_numa1`

Два режима раскладки CPU:
- `interleave`: потоки сначала раскладываются по разным CCD внутри NUMA
- `dense`: потоки плотно упаковываются в последовательные CPU внутри NUMA

Цель:
- проверить, связан ли knee около 12 потоков с текущим interleave-порядком по CCD
