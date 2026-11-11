#!/usr/bin/env python3
import re,sys
rows=[]
cur={}
for l in open(sys.argv[1],errors='ignore'):
    m=re.search(r'prof2 ms/frame: rdptri (\d+) .*track (\d+) nontrack (\d+) effects (\d+)',l)
    if m: cur=dict(rdptri=int(m[1]),track=int(m[2]),non=int(m[3]),fx=int(m[4]))
    m=re.search(r'counts/frame: actors (\d+) renderBegin (\d+) rdptris (\d+)',l)
    if m: cur['tris']=int(m[3])
    m=re.search(r'whole loop iteration (\d+)',l)
    if m and cur.get('tris',0)>100:
        cur['loop']=int(m[1]); rows.append(cur); cur={}
print(len(rows),'race samples')
if rows:
    for k in ('loop','rdptri','track','non','fx','tris'):
        print(k, sum(r[k] for r in rows)//len(rows), end=' | ')
    print()
