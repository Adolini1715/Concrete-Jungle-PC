import csv, json, bisect, collections, re, sys
rows=list(csv.DictReader(open('/home/claude/pcj-decomp/symbols/functions.csv')))
fn=sorted(((int(r["va"],16),int(r["size"] or 0),r) for r in rows),key=lambda z:z[0])
starts=[f[0] for f in fn]
def owner(a):
    i=bisect.bisect_right(starts,a)-1
    if i>=0 and a<fn[i][0]+max(fn[i][1],1): return fn[i][2]
    return None
x=json.load(open('/home/claude/pcj-native/work/disasm/xrefs.json'))
GLIB={'d3d8','xgraphics','d3dx8'}
callers=collections.defaultdict(set); sites=collections.Counter()
for e in x:
    if e['type'] not in ('call','jmp','jump','data','ptr'): pass
    t=owner(int(e['to'],16)); f=owner(int(e['from'],16))
    if not t or t['lib'] not in GLIB: continue
    if int(e['to'],16)!=int(t['va'],16): continue
    if f and f['lib'] in GLIB: continue
    callers[t['va']].add(f['name'] if f else e['from']); sites[t['va']]+=1
byva={r['va']:r for r in rows}
out=collections.defaultdict(list)
for va,n in sites.items():
    r=byva[va]; out[r['lib']].append((r['name'],va,n,len(callers[va])))
tot={l:sum(1 for r in rows if r['lib']==l) for l in GLIB}
for l in sorted(out):
    print(f"## {l}: {len(out[l])} of {tot[l]} functions referenced from game/engine code")
    for name,va,n,c in sorted(out[l],key=lambda z:-z[2]): print(f"{l}\t{va}\t{name}\t{n}\t{c}")
# engine-side wrappers named D3D*
w=[r for r in rows if r['lib']=='enginex' and re.search(r'D3D|Direct3D',r['name'])]
print("## enginex inline D3D wrappers:",len(w))
for r in w: print("W\t"+r['va']+"\t"+r['name'])
