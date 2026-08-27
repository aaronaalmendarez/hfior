#!/usr/bin/env python3
from __future__ import annotations
import csv, json, pathlib
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
ROOT=pathlib.Path(__file__).resolve().parents[2]
OUT=ROOT/'benchmarks/plots';OUT.mkdir(parents=True,exist_ok=True)
# Physical game FPS comparison. Bars show medians; dots show every valid run.
game_results=ROOT/'benchmarks/raw/physical-game-8k'
with (game_results/'aggregate.json').open() as stream:
 physical=json.load(stream)

def load_game_runs(prefix):
 runs=[]
 for path in sorted(game_results.glob(f'{prefix}-*.csv.summary.json')):
  with path.open() as stream:
   run=json.load(stream)
  if run['valid']:
   runs.append(run)
 return runs

standard_runs=load_game_runs('eager')
hfior_runs=load_game_runs('hfior')
standard=physical['modes']['eager-same-source']
hfior=physical['modes']['hfior-late-latch']
comparison=physical['comparison']
fps_metrics=[
 ('Average FPS','average_fps','median_average_fps','average_fps_change_percent'),
 ('1% low FPS','one_percent_low_fps','median_one_percent_low_fps','one_percent_low_change_percent'),
 ('0.1% low FPS','zero_point_one_percent_low_fps','median_zero_point_one_percent_low_fps','zero_point_one_percent_low_change_percent'),
]
bg='#07111f';panel='#0b1726';text='#f4f7fb';muted='#9aabc0';grid='#24364a'
standard_color='#ff7f87';hfior_color='#48e0c2'
fig,ax=plt.subplots(figsize=(10,6),facecolor=bg)
ax.set_facecolor(panel)
x_positions=[0,1,2];bar_width=.34;jitter=[-.055,-.028,0,.028,.055]
standard_positions=[x-bar_width/2 for x in x_positions]
hfior_positions=[x+bar_width/2 for x in x_positions]
standard_medians=[float(standard[item[2]]) for item in fps_metrics]
hfior_medians=[float(hfior[item[2]]) for item in fps_metrics]
ax.bar(standard_positions,standard_medians,width=bar_width,color=standard_color,label='Standard 8K',zorder=2)
ax.bar(hfior_positions,hfior_medians,width=bar_width,color=hfior_color,label='HFIOR',zorder=2)
for index,item in enumerate(fps_metrics):
 run_key=item[1]
 ax.scatter([standard_positions[index]+offset for offset in jitter],
            [float(run[run_key]) for run in standard_runs],
            s=30,color=text,edgecolor=bg,linewidth=.7,zorder=4)
 ax.scatter([hfior_positions[index]+offset for offset in jitter],
            [float(run[run_key]) for run in hfior_runs],
            s=30,color=text,edgecolor=bg,linewidth=.7,zorder=4)
 standard_max=max(float(run[run_key]) for run in standard_runs)
 hfior_max=max(float(run[run_key]) for run in hfior_runs)
 ax.text(standard_positions[index],standard_max+2.2,
         f'{standard_medians[index]:.1f}',ha='center',color=standard_color,
         fontsize=11,fontweight='bold')
 ax.text(hfior_positions[index],hfior_max+2.2,
         f'{hfior_medians[index]:.1f}',ha='center',color=hfior_color,
         fontsize=11,fontweight='bold')
 change=float(comparison[item[3]])
 ax.text(x_positions[index],max(standard_max,hfior_max)+10,
         f'+{change:.2f}%',ha='center',color=hfior_color,
         fontsize=11,fontweight='bold')
ax.set_ylim(0,135)
ax.set_xticks(x_positions,labels=[item[0] for item in fps_metrics])
ax.set_ylabel('Frames per second',color=muted,labelpad=12)
ax.tick_params(axis='x',colors=text,labelsize=12,length=0,pad=12)
ax.tick_params(axis='y',colors=muted)
ax.yaxis.grid(True,color=grid,linewidth=1,alpha=.7)
ax.xaxis.grid(False)
for spine in ax.spines.values():spine.set_visible(False)
legend=ax.legend(loc='upper left',bbox_to_anchor=(0,1.16),ncol=2,frameon=False,fontsize=11)
for label in legend.get_texts():label.set_color(text)
fig.text(.09,.94,'FPS in the Heavy 3D Test',color=text,fontsize=23,fontweight='bold',ha='left')
fig.text(.09,.885,'Bars show the median. Dots show all 5 real-mouse runs. Higher is better.',color=muted,fontsize=11,ha='left')
fig.text(.09,.035,'8 kHz mouse · 65,536 objects · 8,192 camera calculations · 128 drawing passes',color=muted,fontsize=9.5,ha='left')
fig.text(.91,.035,'Input-sensitive stress test',color=muted,fontsize=9.5,ha='right')
fig.subplots_adjust(left=.12,right=.94,bottom=.17,top=.76)
fig.savefig(OUT/'physical-game-fps.png',dpi=180,facecolor=fig.get_facecolor())
plt.close(fig)
# Controlled 100 us integration trace.
rows=list(csv.DictReader((ROOT/'benchmarks/processed/hfior/summary.csv').open()))
order=['eager-thread','hfior-late','hfior-late-latch','hfior-8-latch']
by={r['policy']:r for r in rows}
labels=['Standard eager','HFIOR frame read','HFIOR final check','HFIOR repeated checks']
values=[float(by[p]['aggregate_age_p99_ns'])/1000 for p in order]
fig,ax=plt.subplots(figsize=(9,5.5))
ax.bar(labels,values)
ax.set_ylabel('Slow-end input age in microseconds (lower is better)')
ax.set_title('Input Age Near the Frame Use Point (8 kHz Mouse, 240 Hz Game)')
for i,v in enumerate(values):ax.text(i,v,f'{v:.0f}',ha='center',va='bottom')
fig.tight_layout();fig.savefig(OUT/'late-latch-p99.png',dpi=180);plt.close(fig)
# Fitted action scaling.
models=list(csv.DictReader((ROOT/'benchmarks/processed/hfior/action-scaling-model.csv').open()))
rates=[1000,2000,4000,8000,16000,32000,64000]
display_labels={
 'eager-thread':'Eager',
 'hfior-8-latch':'HFIOR final checks',
 'hfior-critical-ack':'HFIOR basic read',
}
fig,ax=plt.subplots(figsize=(9,5.5))
for r in models:
 intercept=float(r['action_intercept_per_s']);slope=float(r['actions_per_report_slope'])
 ax.plot(rates,[intercept+slope*x for x in rates],marker='o',label=display_labels.get(r['policy'],r['policy']))
ax.set_xscale('log',base=2);ax.set_yscale('log')
ax.set_xticks(rates,labels=[f'{x//1000}K' for x in rates])
ax.set_xlabel('Mouse report rate')
ax.set_ylabel('Predicted heavy game updates per second')
ax.set_title('How Often the Game Has to React')
ax.legend();ax.grid(True,which='both',axis='both',alpha=.25)
fig.tight_layout();fig.savefig(OUT/'action-scaling-model.png',dpi=180);plt.close(fig)
