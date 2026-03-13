#!/usr/bin/env python3
import argparse
import base64
import concurrent.futures as cf
import json
import subprocess
from pathlib import Path

ROOT = Path('/home/robdrynkin/ydbwork/ydb')
EXP = ROOT / 'ydb/tools/memory_no_ydb_repro'
BENCH = EXP / 'mem_bw_stress/mem_bw_stress'
REMOTE_BENCH = '/tmp/mem_bw_stress'
REMOTE_RUNNER = '/tmp/run_mem_bw_scenario_remote.py'
HOSTS = [
    'node-1302.ydb-dev.ik8s.nebiuscloud.net',
    'node-1306.ydb-dev.ik8s.nebiuscloud.net',
    'node-1308.ydb-dev.ik8s.nebiuscloud.net',
]
THREADS = [1, 2, 4, 8, 12, 16, 24, 32, 48, 64, 96]
SIZES = [4096, 1 << 20]
WORKING_SET = 64 << 20
DURATION = 3
PREWARM = 1.0
NUMA0_RANGE = range(0, 96)
NUMA1_RANGE = range(96, 192)


def run(cmd):
    return subprocess.check_output(cmd, text=True)


def write_payload(path: Path, payload: dict) -> None:
    tmp = path.with_suffix(path.suffix + '.tmp')
    tmp.write_text(json.dumps(payload, indent=2, sort_keys=True))
    tmp.replace(path)


def interleave_ccd(cpu_range):
    cpus = list(cpu_range)
    block = 8
    groups = [cpus[i:i + block] for i in range(0, len(cpus), block)]
    out = []
    for offset in range(block):
        for group in groups:
            if offset < len(group):
                out.append(group[offset])
    return out


ORDERS = {
    'interleave': {
        0: interleave_ccd(NUMA0_RANGE),
        1: interleave_ccd(NUMA1_RANGE),
    },
    'dense': {
        0: list(NUMA0_RANGE),
        1: list(NUMA1_RANGE),
    },
}


def cpu_list(order, threads):
    return ','.join(str(x) for x in order[:threads])


def copy_tools(host):
    subprocess.check_call(['tsh', 'scp', str(BENCH), f'{host}:{REMOTE_BENCH}'], stdout=subprocess.DEVNULL)
    subprocess.check_call(['tsh', 'scp', str(EXP / 'run_scenario_remote.py'), f'{host}:{REMOTE_RUNNER}'], stdout=subprocess.DEVNULL)


def run_one(host, size, threads, numa, packing_mode):
    cmd = {
        'threads': threads,
        'cpu_list': cpu_list(ORDERS[packing_mode][numa], threads),
        'membind': numa,
        'size': size,
        'working_set_bytes': WORKING_SET,
        'warmup_sec': 0,
        'duration_sec': DURATION,
    }
    scenario = f'local_numa{numa}'
    spec = {
        'name': f'{packing_mode}.{scenario}.size{size}.thr{threads}',
        'bench': REMOTE_BENCH,
        'commands': [cmd],
        'uprof_sec': DURATION,
        'prewarm_sec': PREWARM,
        'extra_bench_args': [],
    }
    payload = base64.b64encode(json.dumps(spec).encode()).decode()
    out = run(['tsh', 'ssh', host, '--', f'sudo python3 {REMOTE_RUNNER} {payload}'])
    result = json.loads(out)
    for item in result.get('bench_results', []):
        item.pop('raw_output', None)
    if result.get('uprof'):
        result['uprof'].pop('raw_output', None)
    result['size'] = size
    result['threads_per_process'] = threads
    result['numa'] = numa
    result['packing_mode'] = packing_mode
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--report-dir', required=True)
    args = parser.parse_args()

    report_dir = Path(args.report_dir)
    report_dir.mkdir(parents=True, exist_ok=True)
    out = report_dir / 'local_packing_compare.json'

    for host in HOSTS:
        copy_tools(host)

    results = []
    payload = {
        'hosts': HOSTS,
        'sizes': SIZES,
        'threads': THREADS,
        'working_set_bytes': WORKING_SET,
        'duration_sec': DURATION,
        'prewarm_sec': PREWARM,
        'packing_modes': list(ORDERS),
        'results': results,
    }
    write_payload(out, payload)

    total = len(HOSTS) * len(SIZES) * len(THREADS) * 2 * len(ORDERS)
    index = 0
    for size in SIZES:
        for threads in THREADS:
            for numa in (0, 1):
                for packing_mode in ORDERS:
                    with cf.ThreadPoolExecutor(max_workers=len(HOSTS)) as pool:
                        futs = [pool.submit(run_one, host, size, threads, numa, packing_mode) for host in HOSTS]
                        for fut in cf.as_completed(futs):
                            index += 1
                            result = fut.result()
                            results.append(result)
                            write_payload(out, payload)
                            print(
                                f'[{index}/{total}] {result["host"]} size={size} threads={threads} '
                                f'numa={numa} packing={packing_mode} bench={result["bench_aggregate_traffic_gbps_sum"]:.2f}GB/s',
                                flush=True,
                            )

    write_payload(out, payload)
    print(str(out))


if __name__ == '__main__':
    main()
