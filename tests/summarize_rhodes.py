"""Summarize phase-scoped ESP32 profiling logs; run from repository root."""
import csv
import re
import sys
from pathlib import Path

path = Path(sys.argv[1]) if len(sys.argv)>1 else Path(__file__).parent/'results'/'rhodes-stress.log'
text = path.read_text()
rows = []
for line in text.splitlines():
    if '[Rhodes bench]' not in line:
        continue
    fields = dict(re.findall(r'(\w+)=([\w.]+)', line))
    rows.append({k: v if k == 'phase' else float(v) if k == 'peak' else int(v)
                 for k, v in fields.items()})
assert len(rows) == 45, f'Expected 45 phase records, got {len(rows)}'
assert '[Rhodes stress] DONE' in text
with path.with_suffix('.csv').open('w') as f:
    writer = csv.DictWriter(f, fieldnames=list(rows[0]))
    writer.writeheader()
    writer.writerows(rows)
names = {0:'Rhodes', 5:'Bark', 6:'Velvet Trem'}
print('| 音色 | MIDI開始音 | 音数 | 打鍵 最大% | 保持 平均 / 最大% | 再打鍵 最大% | 期限超過 | Queue overflow |')
print('|---|---|---:|---:|---:|---:|---:|---:|')
groups = sorted({(r.get('preset',0),r['base'],r['voices']) for r in rows})
for preset, base, count in groups:
    records = [r for r in rows if (r.get('preset',0),r['base'],r['voices'])==(preset,base,count)]
    assert len(records)==3
    phases={r['phase']:r for r in records}
    a,s,r = (phases[p] for p in ('attack','sustain','retrigger'))
    missed=sum(p['missed'] for p in records)
    qovf=sum(p['qovf'] for p in records)
    print(f"| {names[preset]} | {base} | {count} | {a['max_us']*.075:.1f} | {s['avg_us']*.075:.1f} / {s['max_us']*.075:.1f} | {r['max_us']*.075:.1f} | {missed} | {qovf} |")
print('\nWorst:',max(rows,key=lambda r:r['max_us']))
print('Total missed:',sum(r['missed'] for r in rows),'queue overflow:',sum(r['qovf'] for r in rows),'over-ceiling samples:',sum(r.get('over',r.get('clip',0)) for r in rows))
