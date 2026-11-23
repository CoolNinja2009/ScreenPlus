"""Builds resources/lang/<code>.txt from resources/lang/keys.en (the English texts, one a line) and a translation
list: one "N|text" line per key, N its line number in keys.en, '@name|Language name' first.

    python scripts/lang-pack.py <code> <translations.tr>

Checks that every key has one translation and that the % placeholders come in the same order (%.0ls swallows a
%ls the language has no use for, such as an English plural "s"). Leading spaces of a key are kept."""
import re, sys, pathlib

root = pathlib.Path(__file__).resolve().parent.parent / 'resources' / 'lang'
keys = (root / 'keys.en').read_text(encoding='utf-8').rstrip('\n').split('\n')
code, src = sys.argv[1], pathlib.Path(sys.argv[2])
spec = re.compile(r'%[-+ #0]*(?:\d+|\*)?(?:\.(?:\d+|\*))?(?:l|h|ll)?[diouxXeEfgGcs%]')

def kinds(s):
    return [m[-1] if m != '%%' else '%' for m in spec.findall(s)]

name, tx, bad = None, {}, []
for line in src.read_text(encoding='utf-8').split('\n'):
    if not line.strip():
        continue
    n, _, t = line.partition('|')
    if n == '@name':
        name = t
        continue
    i = int(n) - 1
    if i in tx or not 0 <= i < len(keys):
        bad.append(f'{n}: duplicate or out of range')
    tx[i] = t.strip()
for i, k in enumerate(keys):
    if i not in tx:
        bad.append(f'{i + 1}: missing ({k})')
    elif kinds(k) != kinds(tx[i]):
        bad.append(f'{i + 1}: placeholders differ: {k!r} -> {tx[i]!r}')
if not name:
    bad.append('no @name line')
if bad:
    sys.exit('\n'.join(bad))
out = [f'# Screen+ UI in {name}: English<TAB>translation (src/common/i18n.c). Made by scripts/lang-pack.py.', f'@name\t{name}']
for i, k in enumerate(keys):
    lead = k[:len(k) - len(k.lstrip(' '))]
    out.append(f'{k}\t{lead}{tx[i]}')
(root / f'{code}.txt').write_text('\n'.join(out) + '\n', encoding='utf-8', newline='\n')
print(f'{code}.txt: {len(keys)} texts')
