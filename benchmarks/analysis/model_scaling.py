#!/usr/bin/env python3
import argparse,csv,json,math,pathlib,statistics

def f(x):
 try:return float(x)
 except:return 0.0

def fit(xs,ys):
 n=len(xs); mx=sum(xs)/n; my=sum(ys)/n
 ss=sum((x-mx)**2 for x in xs)
 b=sum((x-mx)*(y-my) for x,y in zip(xs,ys))/ss if ss else 0
 a=my-b*mx
 pred=[a+b*x for x in xs]
 sst=sum((y-my)**2 for y in ys); sse=sum((y-p)**2 for y,p in zip(ys,pred))
 return a,b,1-sse/sst if sst else 1
ap=argparse.ArgumentParser();ap.add_argument('summary',type=pathlib.Path);ap.add_argument('--output',type=pathlib.Path,required=True);a=ap.parse_args();a.output.mkdir(parents=True,exist_ok=True)
rows=list(csv.DictReader(a.summary.open()))
rate=[r for r in rows if r.get('scenario')=='rate-sweep']
models=[]
for policy in sorted({r['policy'] for r in rate}):
 p=[r for r in rate if r['policy']==policy]; xs=[f(r['synthetic_rate_hz']) for r in p]; ys=[f(r['median_expensive_actions_per_s']) for r in p]
 intercept,slope,r2=fit(xs,ys); models.append({'policy':policy,'action_intercept_per_s':intercept,'actions_per_report_slope':slope,'r_squared':r2})
with (a.output/'action-scaling-model.csv').open('w',newline='') as o:
 w=csv.DictWriter(o,fieldnames=models[0].keys());w.writeheader();w.writerows(models)
ex=[]
for rate_hz in [8000,16000,32000,64000]:
 row={'report_rate_hz':rate_hz}
 for m in models:
  row[m['policy']+'_predicted_actions_per_s']=max(0,m['action_intercept_per_s']+m['actions_per_report_slope']*rate_hz)
 ex.append(row)
with (a.output/'higher-rate-extrapolation.csv').open('w',newline='') as o:
 fields=list(ex[0]);w=csv.DictWriter(o,fieldnames=fields);w.writeheader();w.writerows(ex)
# Consumer-only CPU-time model from observed per-action medians. It explicitly excludes producer/kernel work.
primary=[]
for r in rows:
 actions=f(r.get('median_expensive_actions_per_s')); cpu_pct=f(r.get('median_estimated_transport_cpu_percent'))
 if actions>0: primary.append((r['policy'],cpu_pct/actions))
per={}
for policy in sorted({p for p,_ in primary}): per[policy]=statistics.median(v for p,v in primary if p==policy)
cpu=[]
for row in ex:
 out={'report_rate_hz':row['report_rate_hz']}
 for m in models:
  pol=m['policy']; acts=row[pol+'_predicted_actions_per_s']; out[pol+'_consumer_cpu_percent_estimate']=acts*per.get(pol,0)
 cpu.append(out)
with (a.output/'consumer-cpu-extrapolation.csv').open('w',newline='') as o:
 fields=list(cpu[0]);w=csv.DictWriter(o,fieldnames=fields);w.writeheader();w.writerows(cpu)
meta={'schema':3,'evidence_class':'synthetic-screening-model','limitations':['One short run per screening cell; latency values are diagnostic, not inferential.','CPU extrapolation is consumer-process CPU only and excludes the unavoidable producer/kernel per-observation term.','Hardware cycles/report require physical perf counters; perf was unavailable in the research container.'],'formulae':{'actions':'intercept + slope * report_rate','normalized_cycles':'consumer_cpu_seconds_per_second * chosen_reference_frequency_hz'}}
(a.output/'scaling-model.json').write_text(json.dumps(meta,indent=2)+'\n')
