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


NUMA0_ORDER = interleave_ccd(NUMA0_RANGE)
NUMA1_ORDER = interleave_ccd(NUMA1_RANGE)


def cpu_list(order, threads):
    return ','.join(str(x) for x in order[:threads])


SCENARIO_BUILDERS = {
    'local_numa0': lambda threads, size: [
        {'threads': threads, 'cpu_list': cpu_list(NUMA0_ORDER, threads), 'membind': 0, 'size': size, 'working_set_bytes': WORKING_SET, 'warmup_sec': 0, 'duration_sec': DURATION},
    ],
    'local_numa1': lambda threads, size: [
        {'threads': threads, 'cpu_list': cpu_list(NUMA1_ORDER, threads), 'membind': 1, 'size': size, 'working_set_bytes': WORKING_SET, 'warmup_sec': 0, 'duration_sec': DURATION},
    ],
    'remote_0_to_1': lambda threads, size: [
        {'threads': threads, 'cpu_list': cpu_list(NUMA0_ORDER, threads), 'membind': 1, 'size': size, 'working_set_bytes': WORKING_SET, 'warmup_sec': 0, 'duration_sec': DURATION},
    ],
    'remote_1_to_0': lambda threads, size: [
        {'threads': threads, 'cpu_list': cpu_list(NUMA1_ORDER, threads), 'membind': 0, 'size': size, 'working_set_bytes': WORKING_SET, 'warmup_sec': 0, 'duration_sec': DURATION},
    ],
    'dual_local': lambda threads, size: [
        {'threads': threads, 'cpu_list': cpu_list(NUMA0_ORDER, threads), 'membind': 0, 'size': size, 'working_set_bytes': WORKING_SET, 'warmup_sec': 0, 'duration_sec': DURATION},
        {'threads': threads, 'cpu_list': cpu_list(NUMA1_ORDER, threads), 'membind': 1, 'size': size, 'working_set_bytes': WORKING_SET, 'warmup_sec': 0, 'duration_sec': DURATION},
    ],
    'dual_cross': lambda threads, size: [
        {'threads': threads, 'cpu_list': cpu_list(NUMA0_ORDER, threads), 'membind': 1, 'size': size, 'working_set_bytes': WORKING_SET, 'warmup_sec': 0, 'duration_sec': DURATION},
        {'threads': threads, 'cpu_list': cpu_list(NUMA1_ORDER, threads), 'membind': 0, 'size': size, 'working_set_bytes': WORKING_SET, 'warmup_sec': 0, 'duration_sec': DURATION},
    ],
    'mixed_target_0': lambda threads, size: [
        {'threads': threads, 'cpu_list': cpu_list(NUMA0_ORDER, threads), 'membind': 0, 'size': size, 'working_set_bytes': WORKING_SET, 'warmup_sec': 0, 'duration_sec': DURATION},
        {'threads': threads, 'cpu_list': cpu_list(NUMA1_ORDER, threads), 'membind': 0, 'size': size, 'working_set_bytes': WORKING_SET, 'warmup_sec': 0, 'duration_sec': DURATION},
    ],
    'mixed_target_1': lambda threads, size: [
        {'threads': threads, 'cpu_list': cpu_list(NUMA0_ORDER, threads), 'membind': 1, 'size': size, 'working_set_bytes': WORKING_SET, 'warmup_sec': 0, 'duration_sec': DURATION},
        {'threads': threads, 'cpu_list': cpu_list(NUMA1_ORDER, threads), 'membind': 1, 'size': size, 'working_set_bytes': WORKING_SET, 'warmup_sec': 0, 'duration_sec': DURATION},
    ],
}
SCENARIO_ORDER = list(SCENARIO_BUILDERS)


def run(cmd):
    return subprocess.check_output(cmd, text=True)


def write_payload(path: Path, payload: dict) -> None:
    tmp = path.with_suffix(path.suffix + '.tmp')
    tmp.write_text(json.dumps(payload, indent=2, sort_keys=True))
    tmp.replace(path)


def copy_tools(host):
    subprocess.check_call(['tsh', 'scp', str(BENCH), f'{host}:{REMOTE_BENCH}'], stdout=subprocess.DEVNULL)
    subprocess.check_call(['tsh', 'scp', str(EXP / 'run_scenario_remote.py'), f'{host}:{REMOTE_RUNNER}'], stdout=subprocess.DEVNULL)


def run_one(host, size, threads, scenario_name, extra_bench_args):
    commands = SCENARIO_BUILDERS[scenario_name](threads, size)
    spec = {
        'name': f'{scenario_name}.size{size}.thr{threads}',
        'bench': REMOTE_BENCH,
        'commands': commands,
        'uprof_sec': DURATION,
        'prewarm_sec': PREWARM,
        'extra_bench_args': extra_bench_args,
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
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--report-dir', required=True)
    parser.add_argument('--label', required=True)
    parser.add_argument('extra_bench_args', nargs='*')
    args = parser.parse_args()

    report_dir = Path(args.report_dir)
    report_dir.mkdir(parents=True, exist_ok=True)
    out = report_dir / 'memory_no_ydb_sweep.json'

    for host in HOSTS:
        copy_tools(host)

    all_results = []
    payload = {
        'label': args.label,
        'hosts': HOSTS,
        'sizes': SIZES,
        'threads': THREADS,
        'working_set_bytes': WORKING_SET,
        'duration_sec': DURATION,
        'prewarm_sec': PREWARM,
        'numa0_order': NUMA0_ORDER,
        'numa1_order': NUMA1_ORDER,
        'extra_bench_args': args.extra_bench_args,
        'results': all_results,
    }
    write_payload(out, payload)

    total = len(HOSTS) * len(SIZES) * len(THREADS) * len(SCENARIO_ORDER)
    index = 0
    for size in SIZES:
        for threads in THREADS:
            for scenario_name in SCENARIO_ORDER:
                with cf.ThreadPoolExecutor(max_workers=len(HOSTS)) as pool:
                    futs = [pool.submit(run_one, host, size, threads, scenario_name, args.extra_bench_args) for host in HOSTS]
                    for fut in cf.as_completed(futs):
                        index += 1
                        result = fut.result()
                        all_results.append(result)
                        write_payload(out, payload)
                        print(f'[{index}/{total}] {args.label} {result["host"]} size={size} threads={threads} scenario={scenario_name} bench={result["bench_aggregate_traffic_gbps_sum"]:.2f}GB/s', flush=True)

    write_payload(out, payload)
    print(str(out))


if __name__ == '__main__':
    main()
