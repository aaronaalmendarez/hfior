#!/usr/bin/env python3
"""Aggregate HFIOR runs without treating synthetic data as physical proof."""
from __future__ import annotations
import argparse, bisect, csv, json, math, os, pathlib, re, statistics, sys
from collections import Counter, defaultdict

NUMERIC_FIELDS = {
    'wall_s','frame_hz_target','checks_per_frame','base_work_ns_per_frame',
    'integration_work_ns_per_frame','callback_work_ns_per_record','spin_ns',
    'frames','overruns','average_fps','one_percent_low_fps',
    'zero_point_one_percent_low_fps','frametime_mean_ns','frametime_p50_ns',
    'frametime_p95_ns','frametime_p99_ns','frametime_p99_9_ns',
    'newest_sample_age_p50_ns','newest_sample_age_p90_ns',
    'newest_sample_age_p95_ns','newest_sample_age_p99_ns',
    'newest_sample_age_p99_9_ns','newest_sample_age_max_ns',
    'legacy_pre_latch_age_p50_ns','legacy_pre_latch_age_p99_ns',
    'latch_cost_p50_ns','latch_cost_p99_ns','frame_start_lateness_p50_ns',
    'frame_start_lateness_p99_ns','eager_ingress_age_p50_ns',
    'eager_ingress_age_p99_ns','button_age_p50_ns','button_age_p99_ns',
    'input_consumptions','expensive_actions','expensive_actions_per_s',
    'late_latch_actions','late_latch_records','frames_with_late_latch',
    'ring_checks','nonempty_ring_checks','ring_records','shadow_ring_records',
    'eager_messages','eager_records','acknowledgements_sent','urgent_reads',
    'sequence_gaps','duplicate_or_reordered','producer_ring_drops',
    'producer_eager_drops','planned_work_cpu_ns','estimated_transport_cpu_ns',
    'estimated_transport_cpu_percent','consumer_process_cpu_ns',
    'ingress_thread_cpu_ns','voluntary_context_switches',
    'involuntary_context_switches','context_switches_per_s','generation_changes',
    'stall_count','spin_iterations'
}

def read_json(path: pathlib.Path):
    return json.loads(path.read_text())

def read_env(path: pathlib.Path):
    out={}
    if not path.exists(): return out
    for line in path.read_text(errors='replace').splitlines():
        if '=' in line:
            k,v=line.split('=',1); out[k]=v
    return out

def num(value, default=0.0):
    try: return float(value)
    except (TypeError,ValueError): return default

def percentile(values, p):
    vals=sorted(v for v in values if v is not None and math.isfinite(float(v)))
    if not vals: return 0.0
    rank=(p/100.0)*(len(vals)-1)
    lo=int(math.floor(rank)); hi=int(math.ceil(rank)); f=rank-lo
    return float(vals[lo])*(1-f)+float(vals[hi])*f

def median(values):
    vals=[float(v) for v in values if v is not None and math.isfinite(float(v))]
    return statistics.median(vals) if vals else 0.0

def write_csv(path, rows, fieldnames=None):
    path.parent.mkdir(parents=True, exist_ok=True)
    if fieldnames is None:
        fieldnames=[]
        seen=set()
        for row in rows:
            for k in row:
                if k not in seen: seen.add(k); fieldnames.append(k)
    with path.open('w',newline='') as f:
        w=csv.DictWriter(f,fieldnames=fieldnames,extrasaction='ignore')
        w.writeheader(); w.writerows(rows)

def infer_rep(path):
    m=re.search(r'(?:^|/)rep[_-]?(\d+)(?:/|$)',str(path))
    return int(m.group(1)) if m else 0

def load_frames(path):
    if not path.exists(): return []
    with path.open(newline='') as f:
        out=[]
        for row in csv.DictReader(f):
            converted={}
            for k,v in row.items():
                try: converted[k]=int(v)
                except (ValueError,TypeError): converted[k]=v
            out.append(converted)
        return out

def load_trace(path):
    if not path.exists(): return [],{},[]
    rows=[]; byseq={}
    with path.open(newline='') as f:
        for row in csv.DictReader(f):
            r={k:int(v) for k,v in row.items()}
            if r.get('published',0):
                rows.append(r); byseq[r['sequence']]=r
    rows.sort(key=lambda r:r['publish_end_ns'])
    ends=[r['publish_end_ns'] for r in rows]
    return rows,byseq,ends

def enrich_trace(run, frames, trace_rows, byseq, ends, threshold_ns=500_000):
    enriched=[]; classes=Counter()
    for fr in frames:
        use=int(fr.get('use_time_ns',0)); newest_seq=int(fr.get('newest_sequence',0))
        idx=bisect.bisect_right(ends,use)-1
        committed=trace_rows[idx] if idx>=0 else None
        consumed=byseq.get(newest_seq)
        committed_seq=committed['sequence'] if committed else 0
        committed_ts=committed['source_timestamp_ns'] if committed else 0
        missed=bool(committed and committed_seq>newest_seq)
        source_delay=(consumed['publish_end_ns']-consumed['source_timestamp_ns']) if consumed else 0
        committed_age=(use-committed_ts) if committed and use>=committed_ts else 0
        actual=int(fr.get('actual_age_ns',0))
        arrival_after_latch=bool(committed and committed['publish_end_ns']>int(fr.get('latch_end_ns',0)))
        cls='not-tail'
        if actual>=threshold_ns:
            if source_delay>=threshold_ns//2: cls='source-publication-delay'
            elif int(fr.get('start_lateness_ns',0))>=threshold_ns//2: cls='consumer-started-late'
            elif missed: cls='eager-or-consumer-missed-committed'
            elif int(fr.get('latch_cost_ns',0))>=threshold_ns//2: cls='latch-preemption'
            elif int(fr.get('post_latch_to_use_ns',0))>=threshold_ns//2: cls='post-latch-work'
            elif arrival_after_latch: cls='arrival-after-latch'
            else: cls='unclassified-phase-or-host-jitter'
            classes[cls]+=1
        enriched.append({
            'run_path':run['run_path'],'policy':run['policy'],'rep':run['rep'],
            'frame':fr.get('frame',0),'actual_age_ns':actual,
            'newest_sequence':newest_seq,'latest_committed_sequence':committed_seq,
            'missed_committed':int(missed),'source_publication_delay_ns':source_delay,
            'latest_committed_age_ns':committed_age,
            'arrival_after_latch':int(arrival_after_latch),
            'start_lateness_ns':fr.get('start_lateness_ns',0),
            'latch_cost_ns':fr.get('latch_cost_ns',0),
            'post_latch_to_use_ns':fr.get('post_latch_to_use_ns',0),
            'tail_classification':cls,
        })
    return enriched,classes

def main():
    ap=argparse.ArgumentParser()
    ap.add_argument('raw_root',type=pathlib.Path)
    ap.add_argument('--output',type=pathlib.Path,required=True)
    ap.add_argument('--plots',type=pathlib.Path,required=True)
    ap.add_argument('--tail-threshold-us',type=float,default=500.0)
    args=ap.parse_args()
    args.output.mkdir(parents=True,exist_ok=True); args.plots.mkdir(parents=True,exist_ok=True)
    runs=[]; all_frames=[]; enriched=[]; classes=Counter(); errors=[]
    for summary_path in sorted(args.raw_root.rglob('client/summary.json')):
        run_dir=summary_path.parent.parent
        try:
            s=read_json(summary_path); integ=read_json(run_dir/'integrity.json')
            env=read_env(run_dir/'run.env')
        except Exception as exc:
            errors.append(f'{run_dir}: {exc}'); continue
        row={'run_path':str(run_dir.relative_to(args.raw_root)),'valid':bool(integ.get('valid')),
             'rep':infer_rep(run_dir),'scenario':env.get('scenario',''),
             'rate_label':env.get('rate_label',''),'synthetic_rate_hz':num(env.get('synthetic_rate_hz')),
             'timestamp_mode':env.get('timestamp_mode',''),'source':env.get('source',''),
             'evidence_class':s.get('evidence_class',''),'policy':s.get('policy',''),
             'ack_placement':s.get('ack_placement',''),'observed_reports_per_s':num(integ.get('observed_reports_per_s'))}
        for k,v in s.items(): row[k]=v
        runs.append(row)
        frames=load_frames(summary_path.parent/'frames.csv')
        for fr in frames:
            fr2={'run_path':row['run_path'],'policy':row['policy'],'rep':row['rep'],
                 'scenario':row['scenario'],'rate_label':row['rate_label'],
                 'frame_hz_target':row.get('frame_hz_target',0),
                 'base_work_ns_per_frame':row.get('base_work_ns_per_frame',0),
                 'integration_work_ns_per_frame':row.get('integration_work_ns_per_frame',0),
                 **fr}
            all_frames.append(fr2)
        trace,byseq,ends=load_trace(run_dir/'producer-trace.csv')
        if trace:
            e,c=enrich_trace(row,frames,trace,byseq,ends,int(args.tail_threshold_us*1000))
            enriched.extend(e); classes.update(c)
    valid=[r for r in runs if r['valid']]
    write_csv(args.output/'runs.csv',runs)
    write_csv(args.output/'frames.csv',all_frames)
    if enriched: write_csv(args.output/'enriched-frames.csv',enriched)

    key_fields=['policy','ack_placement','rate_label','synthetic_rate_hz','timestamp_mode',
                'scenario','frame_hz_target','base_work_ns_per_frame',
                'integration_work_ns_per_frame','callback_work_ns_per_record','spin_ns']
    groups=defaultdict(list)
    for r in valid:
        key=tuple(r.get(k,'') for k in key_fields); groups[key].append(r)
    summary=[]
    for key,items in groups.items():
        row=dict(zip(key_fields,key)); row['runs']=len(items)
        metrics=['observed_reports_per_s','expensive_actions_per_s','context_switches_per_s',
                 'estimated_transport_cpu_ns','estimated_transport_cpu_percent',
                 'consumer_process_cpu_ns','ingress_thread_cpu_ns',
                 'newest_sample_age_p50_ns','newest_sample_age_p95_ns',
                 'newest_sample_age_p99_ns','newest_sample_age_p99_9_ns',
                 'newest_sample_age_max_ns','legacy_pre_latch_age_p50_ns',
                 'legacy_pre_latch_age_p99_ns','latch_cost_p50_ns','latch_cost_p99_ns',
                 'frame_start_lateness_p99_ns','frametime_p99_ns','frametime_p99_9_ns',
                 'late_latch_records','frames_with_late_latch','producer_ring_drops',
                 'sequence_gaps','duplicate_or_reordered']
        for m in metrics: row['median_'+m]=median([i.get(m,0) for i in items])
        paths={i['run_path'] for i in items}
        f=[x for x in all_frames if x['run_path'] in paths and int(x.get('newest_timestamp_ns',0))]
        ages=[int(x['actual_age_ns']) for x in f]
        for p in [50,90,95,99,99.9]: row[f'aggregate_age_p{str(p).replace(".","_")}_ns']=percentile(ages,p)
        row['aggregate_age_max_ns']=max(ages) if ages else 0
        summary.append(row)
    summary.sort(key=lambda r:(str(r.get('rate_label')),str(r.get('scenario')),str(r.get('policy'))))
    write_csv(args.output/'summary.csv',summary)

    # Paired runs: same scenario/rate/frame/work/integration/timestamp/rep, eager as reference.
    match_fields=['scenario','rate_label','frame_hz_target','base_work_ns_per_frame',
                  'integration_work_ns_per_frame','timestamp_mode','rep']
    eager={tuple(r.get(k,'') for k in match_fields):r for r in valid if r['policy']=='eager-thread'}
    comparisons=[]
    for r in valid:
        if r['policy']=='eager-thread': continue
        e=eager.get(tuple(r.get(k,'') for k in match_fields))
        if not e: continue
        row={k:r.get(k,'') for k in match_fields}; row.update({'candidate':r['policy'],'ack_placement':r.get('ack_placement','')})
        for m in ['newest_sample_age_p50_ns','newest_sample_age_p95_ns','newest_sample_age_p99_ns','newest_sample_age_p99_9_ns']:
            row[m+'_delta_ns']=num(r.get(m))-num(e.get(m))
        for m in ['expensive_actions_per_s','context_switches_per_s','estimated_transport_cpu_ns']:
            base=num(e.get(m)); cand=num(r.get(m)); row[m+'_reduction_percent']=((base-cand)/base*100.0) if base else 0.0
        row['candidate_drops']=num(r.get('producer_ring_drops'))
        row['candidate_sequence_errors']=num(r.get('sequence_gaps'))+num(r.get('duplicate_or_reordered'))
        comparisons.append(row)
    write_csv(args.output/'paired-comparisons.csv',comparisons)

    # Policy-level Pareto frontier over latency/actions/context/transport CPU.
    candidates=[]
    for row in summary:
        candidates.append({**row,
          '_p99':num(row.get('aggregate_age_p99_ns')),
          '_actions':num(row.get('median_expensive_actions_per_s')),
          '_ctx':num(row.get('median_context_switches_per_s')),
          '_cpu':num(row.get('median_estimated_transport_cpu_ns'))})
    for a in candidates:
        dominated=False
        for b in candidates:
            if a is b: continue
            av=[a['_p99'],a['_actions'],a['_ctx'],a['_cpu']]; bv=[b['_p99'],b['_actions'],b['_ctx'],b['_cpu']]
            if all(x<=y for x,y in zip(bv,av)) and any(x<y for x,y in zip(bv,av)):
                dominated=True; break
        a['pareto_optimal']=not dominated
    write_csv(args.output/'pareto.csv',[{k:v for k,v in r.items() if not k.startswith('_')} for r in candidates])
    tail_rows=[{'classification':k,'count':v} for k,v in classes.most_common()]
    write_csv(args.output/'tail-classification.csv',tail_rows,['classification','count'])

    plot_files=[]
    try:
        import matplotlib.pyplot as plt
        # Use compact labels because policies repeat across configuration groups.
        labels=[f"{r['policy']}\n{r.get('scenario','')}" for r in summary]
        def bar(metric,name,ylabel,scale=1.0):
            if not summary: return
            vals=[num(r.get(metric))/scale for r in summary]
            fig=plt.figure(figsize=(max(8,len(vals)*0.55),5)); ax=fig.add_subplot(111)
            ax.bar(range(len(vals)),vals); ax.set_xticks(range(len(vals)),labels,rotation=70,ha='right')
            ax.set_ylabel(ylabel); ax.set_title(name.replace('-',' ').title()); fig.tight_layout()
            path=args.plots/(name+'.png'); fig.savefig(path,dpi=160); plt.close(fig); plot_files.append(str(path))
        bar('aggregate_age_p99_ns','sample-age-p99','µs',1000.0)
        bar('aggregate_age_p99_9_ns','sample-age-p999','µs',1000.0)
        bar('median_expensive_actions_per_s','actions-per-second','actions/s')
        bar('median_context_switches_per_s','context-switches-per-second','context switches/s')
        bar('median_estimated_transport_cpu_percent','transport-cpu','estimated CPU %')
        bar('median_latch_cost_p99_ns','latch-cost-p99','µs',1000.0)
        if summary:
            fig=plt.figure(figsize=(7,5)); ax=fig.add_subplot(111)
            for r in summary:
                ax.scatter(num(r.get('median_expensive_actions_per_s')),num(r.get('aggregate_age_p99_ns'))/1000.0)
                ax.annotate(r['policy'],(num(r.get('median_expensive_actions_per_s')),num(r.get('aggregate_age_p99_ns'))/1000.0),fontsize=7)
            ax.set_xlabel('Expensive actions/s'); ax.set_ylabel('Aggregate p99 age (µs)'); ax.set_title('Latency–Action Pareto Plane'); fig.tight_layout()
            path=args.plots/'pareto-latency-actions.png'; fig.savefig(path,dpi=160); plt.close(fig); plot_files.append(str(path))
        if tail_rows:
            fig=plt.figure(figsize=(8,5)); ax=fig.add_subplot(111)
            ax.bar(range(len(tail_rows)),[r['count'] for r in tail_rows]); ax.set_xticks(range(len(tail_rows)),[r['classification'] for r in tail_rows],rotation=55,ha='right')
            ax.set_ylabel('Tail frames'); ax.set_title('Tail Classification'); fig.tight_layout()
            path=args.plots/'tail-classification.png'; fig.savefig(path,dpi=160); plt.close(fig); plot_files.append(str(path))
    except Exception as exc:
        errors.append(f'plotting:{exc}')

    verdict={
      'schema':3,'evidence_class':'synthetic-hfior-analysis',
      'runs_discovered':len(runs),'runs_analyzed':len(runs),'valid_runs':len(valid),
      'invalid_runs':len(runs)-len(valid),'analysis_errors':errors,
      'total_drops':sum(int(num(r.get('producer_ring_drops'))) for r in valid),
      'total_sequence_errors':sum(int(num(r.get('sequence_gaps'))+num(r.get('duplicate_or_reordered'))) for r in valid),
      'pareto_optimal_policies':sorted({r['policy'] for r in candidates if r['pareto_optimal']}),
      'plots':plot_files,'physical_success_level':'UNAWARDED',
      'physical_reason':'This verdict covers synthetic evidence only; physical game evidence is reported separately.'
    }
    (args.output/'analysis-verdict.json').write_text(json.dumps(verdict,indent=2)+'\n')
    print(json.dumps(verdict,indent=2))

if __name__=='__main__': main()
