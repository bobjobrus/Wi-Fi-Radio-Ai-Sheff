# Разворачивает системный профиль Bambu Studio (цепочка inherits + шаблоны g-code из include) в полный JSON для командной строки.
import json, os, sys
R = '/Applications/BambuStudio.app/Contents/Resources/profiles/BBL'
def index(kind):
    idx = {}
    for root, _, files in os.walk(os.path.join(R, kind)):
        for f in files:
            if f.endswith('.json'):
                p = os.path.join(root, f)
                try: d = json.load(open(p, encoding='utf-8'))
                except Exception: continue
                if 'name' in d: idx[d['name']] = p
    return idx
def resolve(kind, name, idx):
    d = json.load(open(idx[name], encoding='utf-8'))
    parent = d.get('inherits')
    base = resolve(kind, parent, idx) if parent else {}
    for inc in d.get('include', []):             # шаблоны g-code (start/end/layer change…)
        t = json.load(open(idx[inc], encoding='utf-8'))
        base.update({k: v for k, v in t.items() if k not in ('name', 'instantiation', 'type', 'from', 'setting_id')})
    base.update({k: v for k, v in d.items() if k != 'include'})
    return base
kind, name, out = sys.argv[1], sys.argv[2], sys.argv[3]
over = json.loads(sys.argv[4]) if len(sys.argv) > 4 else {}
idx = index(kind)
d = resolve(kind, name, idx)
d.pop('inherits', None)
d['from'] = 'system' if not over else 'User'
d.update(over)
json.dump(d, open(out, 'w', encoding='utf-8'), ensure_ascii=False, indent=1)
print(kind, name, '→', len(d), 'ключей', {k: d.get(k) for k in ('printable_area', 'wall_loops', 'sparse_infill_density', 'nozzle_diameter', 'filament_type', 'type') if k in d})
