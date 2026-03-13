#!/usr/bin/env python3
import argparse
import json
from collections import defaultdict
from pathlib import Path
from xml.sax.saxutils import escape

HOSTS = [
    'node-1302.ydb-dev.ik8s.nebiuscloud.net',
    'node-1306.ydb-dev.ik8s.nebiuscloud.net',
    'node-1308.ydb-dev.ik8s.nebiuscloud.net',
]
HOST_SHORT = {
    'node-1302.ydb-dev.ik8s.nebiuscloud.net': '1302',
    'node-1306.ydb-dev.ik8s.nebiuscloud.net': '1306',
    'node-1308.ydb-dev.ik8s.nebiuscloud.net': '1308',
}
COLORS = {
    'node-1302.ydb-dev.ik8s.nebiuscloud.net': '#1f77b4',
    'node-1306.ydb-dev.ik8s.nebiuscloud.net': '#d62728',
    'node-1308.ydb-dev.ik8s.nebiuscloud.net': '#2ca02c',
}
SCENARIOS = [
    'local_numa0', 'local_numa1', 'remote_0_to_1', 'remote_1_to_0',
    'dual_local', 'dual_cross', 'mixed_target_0', 'mixed_target_1',
]
LABELS = {
    'local_numa0': 'local numa0',
    'local_numa1': 'local numa1',
    'remote_0_to_1': 'remote 0->1',
    'remote_1_to_0': 'remote 1->0',
    'dual_local': 'dual local',
    'dual_cross': 'dual cross',
    'mixed_target_0': 'mixed target0',
    'mixed_target_1': 'mixed target1',
}


def thread_xs(threads, x0, w):
    if not threads:
        return []
    min_t = min(threads)
    max_t = max(threads)
    if min_t == max_t:
        return [x0 + w / 2 for _ in threads]
    return [x0 + (t - min_t) * w / (max_t - min_t) for t in threads]


def polyline_points(values, xs, y0, h, min_y, max_y):
    pts = []
    for x, v in zip(xs, values):
        if v is None:
            pts.append(None)
            continue
        y = y0 + h - ((v - min_y) / (max_y - min_y) if max_y > min_y else 0.5) * h
        pts.append((x, y))
    return pts


def nice_max(v):
    if v <= 0:
        return 1
    if v <= 2:
        step = 0.25
    elif v <= 5:
        step = 0.5
    elif v <= 10:
        step = 1
    elif v <= 25:
        step = 2.5
    elif v <= 50:
        step = 5
    elif v <= 100:
        step = 10
    elif v <= 250:
        step = 25
    elif v <= 500:
        step = 50
    else:
        step = 100
    return step * int((v + step - 1e-12) / step + 0.999999)


def render_grid(threads, metric_data, size, out_path, title, y_label):
    width, height = 1600, 1800
    margin_left, margin_top = 90, 110
    gap_x, gap_y = 60, 80
    cols, rows_n = 2, 4
    plot_w = (width - margin_left - 80 - gap_x) / cols
    plot_h = (height - margin_top - 120 - gap_y * (rows_n - 1)) / rows_n

    scenario_max = {}
    for scenario in SCENARIOS:
        mx = 0.0
        for host in HOSTS:
            vals = [metric_data[(size, scenario, host)].get(t) for t in threads if metric_data[(size, scenario, host)].get(t) is not None]
            if vals:
                mx = max(mx, max(vals))
        scenario_max[scenario] = nice_max(mx * 1.05 if mx else 1.0)

    parts = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}">']
    parts.append('<style>text{font-family:monospace;fill:#111}.small{font-size:14px}.axis{stroke:#444;stroke-width:1}.grid{stroke:#ddd;stroke-width:1}</style>')
    parts.append(f'<rect x="0" y="0" width="{width}" height="{height}" fill="white"/>')
    parts.append(f'<text x="{width/2}" y="40" text-anchor="middle" font-size="24">{escape(title)}</text>')
    parts.append(f'<text x="{width/2}" y="70" text-anchor="middle" font-size="16">threads per process on X axis</text>')

    lx = width - 280
    ly = 30
    for i, host in enumerate(HOSTS):
        y = ly + i * 24
        parts.append(f'<line x1="{lx}" y1="{y}" x2="{lx+28}" y2="{y}" stroke="{COLORS[host]}" stroke-width="3"/>')
        parts.append(f'<text x="{lx+36}" y="{y+5}" font-size="14">{HOST_SHORT[host]}</text>')

    for idx, scenario in enumerate(SCENARIOS):
        r, c = divmod(idx, cols)
        x = margin_left + c * (plot_w + gap_x)
        y = margin_top + r * (plot_h + gap_y)
        inner_x = x + 55
        inner_y = y + 25
        inner_w = plot_w - 70
        inner_h = plot_h - 60
        max_y = scenario_max[scenario]
        parts.append(f'<text x="{x + plot_w/2}" y="{y}" text-anchor="middle" font-size="18">{escape(LABELS[scenario])}</text>')
        for j in range(6):
            gy = inner_y + j * inner_h / 5
            val = max_y - j * max_y / 5
            label = f'{val:.2f}'.rstrip('0').rstrip('.')
            parts.append(f'<line class="grid" x1="{inner_x}" y1="{gy}" x2="{inner_x + inner_w}" y2="{gy}"/>')
            parts.append(f'<text class="small" x="{inner_x - 8}" y="{gy + 4}" text-anchor="end">{label}</text>')
        parts.append(f'<line class="axis" x1="{inner_x}" y1="{inner_y + inner_h}" x2="{inner_x + inner_w}" y2="{inner_y + inner_h}"/>')
        parts.append(f'<line class="axis" x1="{inner_x}" y1="{inner_y}" x2="{inner_x}" y2="{inner_y + inner_h}"/>')
        xs = thread_xs(threads, inner_x, inner_w)
        for tx, t in zip(xs, threads):
            parts.append(f'<line class="grid" x1="{tx}" y1="{inner_y + inner_h}" x2="{tx}" y2="{inner_y + inner_h + 5}"/>')
            parts.append(f'<text class="small" x="{tx}" y="{inner_y + inner_h + 20}" text-anchor="middle">{t}</text>')
        for host in HOSTS:
            vals = [metric_data[(size, scenario, host)].get(t) for t in threads]
            pts = [p for p in polyline_points(vals, xs, inner_y, inner_h, 0.0, max_y) if p is not None]
            if pts:
                parts.append(f'<polyline fill="none" stroke="{COLORS[host]}" stroke-width="2.5" points="' + ' '.join(f'{px:.1f},{py:.1f}' for px,py in pts) + '"/>')
                for px, py in pts:
                    parts.append(f'<circle cx="{px:.1f}" cy="{py:.1f}" r="2.8" fill="{COLORS[host]}"/>')
        parts.append(f'<text class="small" x="{x + 10}" y="{y + plot_h/2}" transform="rotate(-90 {x + 10},{y + plot_h/2})" text-anchor="middle">{escape(y_label)}</text>')
    parts.append('</svg>')
    out_path.write_text(''.join(parts))


def render_mixed_components(threads, local_component, remote_component, size, scenario, out_path):
    width, height = 1100, 650
    x0, y0, w, h = 90, 80, 920, 460
    size_label = '4KiB' if size == 4096 else '1MiB'
    max_y = 0.0
    for host in HOSTS:
        for d in (local_component, remote_component):
            vals = [d[(size, scenario, host)].get(t) for t in threads if d[(size, scenario, host)].get(t) is not None]
            if vals:
                max_y = max(max_y, max(vals))
    max_y = nice_max(max_y * 1.05 if max_y else 1.0)
    parts = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}">']
    parts.append('<style>text{font-family:monospace;fill:#111}.small{font-size:14px}.axis{stroke:#444;stroke-width:1}.grid{stroke:#ddd;stroke-width:1}</style>')
    parts.append(f'<rect x="0" y="0" width="{width}" height="{height}" fill="white"/>')
    parts.append(f'<text x="{width/2}" y="36" text-anchor="middle" font-size="24">{escape(LABELS[scenario])} components ({size_label})</text>')
    for j in range(6):
        gy = y0 + j * h / 5
        val = max_y - j * max_y / 5
        parts.append(f'<line class="grid" x1="{x0}" y1="{gy}" x2="{x0+w}" y2="{gy}"/>')
        parts.append(f'<text class="small" x="{x0-8}" y="{gy+4}" text-anchor="end">{val:.0f}</text>')
    parts.append(f'<line class="axis" x1="{x0}" y1="{y0+h}" x2="{x0+w}" y2="{y0+h}"/>')
    parts.append(f'<line class="axis" x1="{x0}" y1="{y0}" x2="{x0}" y2="{y0+h}"/>')
    xs = thread_xs(threads, x0, w)
    for tx, t in zip(xs, threads):
        parts.append(f'<text class="small" x="{tx}" y="{y0+h+20}" text-anchor="middle">{t}</text>')
    legend_y = 52
    for i, host in enumerate(HOSTS):
        base_x = 90 + i * 300
        color = COLORS[host]
        parts.append(f'<line x1="{base_x}" y1="{legend_y}" x2="{base_x+28}" y2="{legend_y}" stroke="{color}" stroke-width="3"/>')
        parts.append(f'<text x="{base_x+34}" y="{legend_y+5}" font-size="14">{HOST_SHORT[host]} local</text>')
        parts.append(f'<line x1="{base_x+120}" y1="{legend_y}" x2="{base_x+148}" y2="{legend_y}" stroke="{color}" stroke-width="2" stroke-dasharray="6,4"/>')
        parts.append(f'<text x="{base_x+154}" y="{legend_y+5}" font-size="14">{HOST_SHORT[host]} remote</text>')
    for host in HOSTS:
        for d, dash in ((local_component, ''), (remote_component, ' stroke-dasharray="6,4"')):
            vals = [d[(size, scenario, host)].get(t) for t in threads]
            pts = [p for p in polyline_points(vals, xs, y0, h, 0.0, max_y) if p is not None]
            if pts:
                parts.append(f'<polyline fill="none" stroke="{COLORS[host]}" stroke-width="2.5"{dash} points="' + ' '.join(f'{px:.1f},{py:.1f}' for px,py in pts) + '"/>')
                for px, py in pts:
                    parts.append(f'<circle cx="{px:.1f}" cy="{py:.1f}" r="2.5" fill="{COLORS[host]}"/>')
    parts.append('</svg>')
    out_path.write_text(''.join(parts))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--report-dir', required=True)
    args = parser.parse_args()

    report_dir = Path(args.report_dir)
    payload = json.loads((report_dir / 'memory_no_ydb_sweep.json').read_text())
    threads = payload['threads']
    rows = payload['results']

    bench = defaultdict(dict)
    uprof = defaultdict(dict)
    uprof_per_thread = defaultdict(dict)
    local_component = defaultdict(dict)
    remote_component = defaultdict(dict)

    for row in rows:
        scenario = row['scenario'].split('.size', 1)[0]
        key = (row['size'], scenario, row['host'])
        bench[key][row['threads_per_process']] = row['bench_aggregate_traffic_gbps_sum']
        u = row.get('uprof') or {}
        if u.get('total_bw_gbps') is not None:
            uprof[key][row['threads_per_process']] = u['total_bw_gbps']
            uprof_per_thread[key][row['threads_per_process']] = u['total_bw_gbps'] / (row['threads_per_process'] * len(row['commands']))
        if scenario in {'mixed_target_0', 'mixed_target_1'} and len(row['bench_results']) == 2:
            if scenario == 'mixed_target_0':
                local = row['bench_results'][0]['traffic_gb_per_sec']
                remote = row['bench_results'][1]['traffic_gb_per_sec']
            else:
                local = row['bench_results'][1]['traffic_gb_per_sec']
                remote = row['bench_results'][0]['traffic_gb_per_sec']
            local_component[key][row['threads_per_process']] = local
            remote_component[key][row['threads_per_process']] = remote

    for size in payload['sizes']:
        size_label = '4KiB' if size == 4096 else '1MiB'
        render_grid(threads, bench, size, report_dir / f'mem_bw_sweep_bench_{size_label}.svg', f'mem_bw_stress aggregate throughput vs threads ({size_label})', 'bench traffic GB/s')
        render_grid(threads, uprof, size, report_dir / f'mem_bw_sweep_uprof_{size_label}.svg', f'uProf total memory bandwidth vs threads ({size_label})', 'uProf total BW GB/s')
        render_grid(threads, uprof_per_thread, size, report_dir / f'mem_bw_sweep_uprof_per_thread_{size_label}.svg', f'uProf memory bandwidth per thread vs threads ({size_label})', 'uProf GB/s per thread')
        for scenario in ('mixed_target_0', 'mixed_target_1'):
            render_mixed_components(threads, local_component, remote_component, size, scenario, report_dir / f'mem_bw_sweep_components_{scenario}_{size_label}.svg')

    print('ok')


if __name__ == '__main__':
    main()
