import io, re
p = '_atl_build.asm'
s = io.open(p, encoding='utf-8', newline='').read()

# atlc 给运行时模块也发射了 fallback __atl_gc_roots（__atl_global_0 单区间）。
# 若保留，atl_rt.obj 里的强定义会压过用户模块的真实根区间表（链接 first-wins）。
# 删除它，让 atl_rt.obj 对 __atl_gc_roots 保持未定义引用，由用户模块解析；
# 用户模块未发射时由 at_gc_stub.obj / gc.obj 的弱定义兜底。
i = s.find('global __atl_gc_roots')
if i >= 0:
    j = s.find('section', i)
    if j > i:
        s = s[:i] + s[j:]

defs = set()
for ln in s.splitlines(True):
    t = ln.strip()
    m = re.match(r'^([A-Za-z_.][A-Za-z0-9_.]*):\s', t)
    if m:
        defs.add(m.group(1))
out = []
for ln in s.splitlines(True):
    t = ln.strip()
    m = re.match(r'^extern\s+([A-Za-z_.][A-Za-z0-9_.]*)\s*$', t)
    if m and m.group(1) in defs:
        continue
    out.append(ln)
io.open(p, 'w', encoding='utf-8', newline='').write(''.join(out))
print('extern-dedup done')
