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
MODES = ['interleave', 'dense']
MODE_LABEL = {
    'interleave': 'interleave CCD',
    'dense': 'dense packing',
}
MODE_DASH = {
    'interleave': '',
    'dense': ' stroke-dasharray="7,4"',
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
    width, height = 1400, 1000
    margin_left, margin_top = 90, 110
    gap_x = 60
    cols = 2
    plot_w = (width - margin_left - 80 - gap_x) / cols
    plot_h = height - margin_top - 120

    subplot_max = {}
    for numa in (0, 1):
        mx = 0.0
        for host in HOSTS:
            for mode in MODES:
                vals = [metric_data[(size, numa, host, mode)].get(t) for t in threads if metric_data[(size, numa, host, mode)].get(t) is not None]
                if vals:
                    mx = max(mx, max(vals))
        subplot_max[numa] = nice_max(mx * 1.05 if mx else 1.0)

    parts = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}">']
    parts.append('<style>text{font-family:monospace;fill:#111}.small{font-size:14px}.axis{stroke:#444;stroke-width:1}.grid{stroke:#ddd;stroke-width:1}</style>')
    parts.append(f'<rect x="0" y="0" width="{width}" height="{height}" fill="white"/>')
    parts.append(f'<text x="{width/2}" y="40" text-anchor="middle" font-size="24">{escape(title)}</text>')
    parts.append(f'<text x="{width/2}" y="68" text-anchor="middle" font-size="16">solid = interleave CCD, dashed = dense packing</text>')

    lx = width - 360
    ly = 28
    for i, host in enumerate(HOSTS):
        y = ly + i * 24
        parts.append(f'<line x1="{lx}" y1="{y}" x2="{lx+28}" y2="{y}" stroke="{COLORS[host]}" stroke-width="3"/>')
        parts.append(f'<text x="{lx+36}" y="{y+5}" font-size="14">{HOST_SHORT[host]}</text>')
    parts.append(f'<line x1="{lx+120}" y1="{ly}" x2="{lx+148}" y2="{ly}" stroke="#444" stroke-width="2"/>')
    parts.append(f'<text x="{lx+156}" y="{ly+5}" font-size="14">{MODE_LABEL["interleave"]}</text>')
    parts.append(f'<line x1="{lx+120}" y1="{ly+24}" x2="{lx+148}" y2="{ly+24}" stroke="#444" stroke-width="2" stroke-dasharray="7,4"/>')
    parts.append(f'<text x="{lx+156}" y="{ly+29}" font-size="14">{MODE_LABEL["dense"]}</text>')

    for idx, numa in enumerate((0, 1)):
        x = margin_left + idx * (plot_w + gap_x)
        y = margin_top
        inner_x = x + 55
        inner_y = y + 25
        inner_w = plot_w - 70
        inner_h = plot_h - 60
        max_y = subplot_max[numa]
        parts.append(f'<text x="{x + plot_w/2}" y="{y}" text-anchor="middle" font-size="18">local numa{numa}</text>')
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
            for mode in MODES:
                vals = [metric_data[(size, numa, host, mode)].get(t) for t in threads]
                pts = [p for p in polyline_points(vals, xs, inner_y, inner_h, 0.0, max_y) if p is not None]
                if pts:
                    parts.append(
                        f'<polyline fill="none" stroke="{COLORS[host]}" stroke-width="2.5"{MODE_DASH[mode]} points="' +
                        ' '.join(f'{px:.1f},{py:.1f}' for px, py in pts) + '"/>'
                    )
                    for px, py in pts:
                        parts.append(f'<circle cx="{px:.1f}" cy="{py:.1f}" r="2.8" fill="{COLORS[host]}"/>')
        parts.append(f'<text class="small" x="{x + 10}" y="{y + plot_h/2}" transform="rotate(-90 {x + 10},{y + plot_h/2})" text-anchor="middle">{escape(y_label)}</text>')
    parts.append('</svg>')
    out_path.write_text(''.join(parts))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--report-dir', required=True)
    args = parser.parse_args()

    report_dir = Path(args.report_dir)
    payload = json.loads((report_dir / 'local_packing_compare.json').read_text())
    threads = payload['threads']
    rows = payload['results']

    bench = defaultdict(dict)
    uprof = defaultdict(dict)
    uprof_per_thread = defaultdict(dict)

    for row in rows:
        key = (row['size'], row['numa'], row['host'], row['packing_mode'])
        bench[key][row['threads_per_process']] = row['bench_aggregate_traffic_gbps_sum']
        u = row.get('uprof') or {}
        if u.get('total_bw_gbps') is not None:
            uprof[key][row['threads_per_process']] = u['total_bw_gbps']
            uprof_per_thread[key][row['threads_per_process']] = u['total_bw_gbps'] / row['threads_per_process']

    for size in payload['sizes']:
        size_label = '4KiB' if size == 4096 else '1MiB'
        render_grid(
            threads,
            bench,
            size,
            report_dir / f'local_packing_bench_{size_label}.svg',
            f'local NUMA throughput: interleave CCD vs dense packing ({size_label})',
            'bench traffic GB/s',
        )
        render_grid(
            threads,
            uprof,
            size,
            report_dir / f'local_packing_uprof_{size_label}.svg',
            f'local NUMA total memory BW: interleave CCD vs dense packing ({size_label})',
            'uProf total BW GB/s',
        )
        render_grid(
            threads,
            uprof_per_thread,
            size,
            report_dir / f'local_packing_uprof_per_thread_{size_label}.svg',
            f'local NUMA memory BW per thread: interleave CCD vs dense packing ({size_label})',
            'uProf GB/s per thread',
        )

    print('ok')


if __name__ == '__main__':
    main()
