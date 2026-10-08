"""Summarize a gta4_metal_frame_log CSV: fps, frame-time percentiles, spikes, and per-5s windows.
usage: summarize_frames.py <frames.csv>     (newest session: ls -t ~/Library/Application\\ Support/LibertyRecomp/perf/)
Columns per window: fps, p95 frame ms, draws, renderer submit ms, time inside Present ms, GPU busy ms, pipelines built.
"""
import csv,statistics as st,sys
rows=[{k:float(v) for k,v in r.items()} for r in csv.DictReader(open(sys.argv[1]))]
rows=[r for r in rows if r['interval_ms']>0]
iv=[r['interval_ms'] for r in rows]; q=sorted(iv); p=lambda x:q[int(len(q)*x)]
print(f"frames {len(rows)} over {rows[-1]['at_ms']/1000:.0f}s | mean {st.mean(iv):.1f} ms ({1000/st.mean(iv):.1f} fps) median {p(.5):.1f} p90 {p(.9):.1f} p99 {p(.99):.1f} max {q[-1]:.0f}")
print(f"spikes>=50ms: {sum(1 for v in iv if v>=50)}  pipeline-wait total {sum(r['pipeline_wait_ms'] for r in rows):.0f} ms, pipelines built {int(sum(r['pipelines_built'] for r in rows))}")
# per 5s windows
w={}
for r in rows:
    k=int(r['at_ms']//5000); w.setdefault(k,[]).append(r)
print("window   fps   p95ms  draws  submit  present  gpu   pipes")
for k in sorted(w):
    rs=w[k]; ivs=sorted(x['interval_ms'] for x in rs)
    print(f"{k*5:4d}s {1000/st.mean(ivs):6.1f} {ivs[int(len(ivs)*.95)]:7.1f} {st.mean(x['draws'] for x in rs):6.0f} {st.mean(x['submit_ms'] for x in rs):7.1f} {st.mean(x['present_ms'] for x in rs):7.1f} {st.mean(x['gpu_ms'] for x in rs):6.1f} {int(sum(x['pipelines_built'] for x in rs)):5d}")
