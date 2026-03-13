#!/usr/bin/env python3
import base64
import json
import re
import subprocess
import sys
from pathlib import Path

if len(sys.argv) != 2:
    print('usage: run_scenario_remote.py <base64-json>', file=sys.stderr)
    sys.exit(2)

spec = json.loads(base64.b64decode(sys.argv[1]).decode())
bench = spec.get('bench', '/tmp/mem_bw_stress')
uprofd = int(spec.get('uprof_sec', max(1, int(spec['commands'][0]['duration_sec']))))
prewarm_sec = float(spec.get('prewarm_sec', 0.0))
extra_bench_args = spec.get('extra_bench_args', [])
name = spec['name']
out_dir = Path('/tmp/mem_bw_runs')
out_dir.mkdir(exist_ok=True)
uprof_out = out_dir / f'{name}.uprof.txt'
bench_outs = [out_dir / f'{name}.bench{i}.txt' for i in range(len(spec['commands']))]
for p in [uprof_out, *bench_outs]:
    try:
        p.unlink()
    except FileNotFoundError:
        pass

procs = []
if shutil := __import__('shutil'):
    if shutil.which('AMDuProfPcm'):
        procs.append(('uprof', subprocess.Popen(['AMDuProfPcm', '--msr', '-m', 'memory', '-a', '-A', 'system', '-d', str(uprofd), '-C'], stdout=open(uprof_out, 'w'), stderr=subprocess.DEVNULL)))


def build_wrapped(cmd, warmup_sec, duration_sec):
    args = [
        bench,
        '--threads', str(cmd['threads']),
        '--cpu-list', cmd['cpu_list'],
        '--size', str(cmd['size']),
        '--working-set-bytes', str(cmd['working_set_bytes']),
        '--warmup-sec', str(warmup_sec),
        '--duration-sec', str(duration_sec),
    ] + list(extra_bench_args)
    wrapped = ['numactl']
    if 'membind' in cmd:
        wrapped += ['--membind', str(cmd['membind'])]
    if 'interleave' in cmd:
        wrapped += ['--interleave', str(cmd['interleave'])]
    wrapped += args
    return wrapped


if prewarm_sec > 0:
    for cmd in spec['commands']:
        subprocess.check_call(build_wrapped(cmd, 0, prewarm_sec), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

for i, cmd in enumerate(spec['commands']):
    wrapped = build_wrapped(cmd, cmd['warmup_sec'], cmd['duration_sec'])
    procs.append((f'bench{i}', subprocess.Popen(wrapped, stdout=open(bench_outs[i], 'w'), stderr=subprocess.STDOUT)))

for _, proc in procs:
    proc.wait()

bench_results = []
for i, path in enumerate(bench_outs):
    text = path.read_text()
    agg = None
    for line in text.splitlines():
        if not line.startswith('aggregate '):
            continue
        row = {}
        for token in line.split()[1:]:
            if '=' not in token:
                continue
            k, v = token.split('=', 1)
            row[k] = v
        agg = row
    if agg is None:
        raise RuntimeError(f'aggregate line not found in {path}')
    bench_results.append({
        'threads': int(agg['threads']),
        'calls': int(agg['calls']),
        'payload_bytes': int(agg['payload_bytes']),
        'traffic_bytes': int(agg['traffic_bytes']),
        'seconds': float(agg['seconds']),
        'payload_gb_per_sec': float(agg['payload_gb_per_sec']),
        'traffic_gb_per_sec': float(agg['traffic_gb_per_sec']),
        'cycles_per_call': float(agg['cycles_per_call']),
        'cycles_per_traffic_byte': float(agg['cycles_per_traffic_byte']),
        'shared_counter': int(agg.get('shared_counter', '0')),
        'raw_output': text,
    })

uprof = None
if uprof_out.exists():
    metrics = {}
    for line in uprof_out.read_text().splitlines():
        if ',' not in line:
            continue
        k, v = line.split(',', 1)
        m = re.search(r'([0-9]+(?:\.[0-9]+)?)', v)
        if m:
            metrics[k.strip()] = float(m.group(1))
    total_bw = metrics.get('Total Mem Bw (GB/s)')
    local_read = metrics.get('Local DRAM Read Data Bytes(GB/s)')
    local_write = metrics.get('Local DRAM Write Data Bytes(GB/s)')
    remote_read = metrics.get('Remote DRAM Read Data Bytes (GB/s)')
    remote_write = metrics.get('Remote DRAM Write Data Bytes (GB/s)')
    remote_pct = ((remote_read or 0.0) + (remote_write or 0.0)) / total_bw * 100.0 if total_bw else None
    uprof = {
        'total_bw_gbps': total_bw,
        'local_read_bw_gbps': local_read,
        'local_write_bw_gbps': local_write,
        'remote_read_bw_gbps': remote_read,
        'remote_write_bw_gbps': remote_write,
        'remote_pct': remote_pct,
        'raw_output': uprof_out.read_text(),
    }

result = {
    'host': subprocess.check_output(['hostname', '-f'], text=True).strip(),
    'scenario': name,
    'commands': spec['commands'],
    'extra_bench_args': extra_bench_args,
    'bench_results': bench_results,
    'bench_aggregate_traffic_gbps_sum': sum(x['traffic_gb_per_sec'] for x in bench_results),
    'bench_aggregate_payload_gbps_sum': sum(x['payload_gb_per_sec'] for x in bench_results),
    'bench_shared_counter_sum': sum(x['shared_counter'] for x in bench_results),
    'uprof': uprof,
}
print(json.dumps(result, sort_keys=True))
