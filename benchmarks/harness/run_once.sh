#!/usr/bin/env bash
set -euo pipefail
ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
BIN_DIR=${HFIOR_BIN_DIR:-"$ROOT/build"}
OUT=
POLICY=
SYNTHETIC_RATE=
DEVICE=
DURATION=2
FRAME_HZ=240
INPUT_PHASE=0.5
BASE_WORK_US=1800
INTEGRATION_WORK_US=0
CALLBACK_WORK_NS=0
ACK_PLACEMENT=post-frame
CHECKS=
FINAL_RECHECKS=
SPIN_US=0
TAIL_THRESHOLD_US=500
TIMESTAMP_MODE=publish
PRODUCER_TRACE=0
RECORD_TRACE=0
CAPACITY=65536
BUTTON_EVERY=0
PRODUCER_CPU=
CONSUMER_CPU=
INGRESS_CPU=
STALL_AFTER_MS=0
STALL_MS=0
SCENARIO=unspecified
RATE_LABEL=
PERF_MODE=off
QUIET=1
SUDO_BRIDGE=0
while (($#)); do
  case "$1" in
    --out) OUT=$2; shift 2;;
    --policy) POLICY=$2; shift 2;;
    --synthetic-rate) SYNTHETIC_RATE=$2; shift 2;;
    --device) DEVICE=$2; shift 2;;
    --duration) DURATION=$2; shift 2;;
    --frame-hz) FRAME_HZ=$2; shift 2;;
    --input-phase) INPUT_PHASE=$2; shift 2;;
    --base-work-us) BASE_WORK_US=$2; shift 2;;
    --integration-work-us) INTEGRATION_WORK_US=$2; shift 2;;
    --callback-work-ns) CALLBACK_WORK_NS=$2; shift 2;;
    --ack-placement) ACK_PLACEMENT=$2; shift 2;;
    --checks-per-frame) CHECKS=$2; shift 2;;
    --final-rechecks) FINAL_RECHECKS=$2; shift 2;;
    --spin-us) SPIN_US=$2; shift 2;;
    --tail-threshold-us) TAIL_THRESHOLD_US=$2; shift 2;;
    --synthetic-timestamps) TIMESTAMP_MODE=$2; shift 2;;
    --producer-trace) PRODUCER_TRACE=1; shift;;
    --no-producer-trace) PRODUCER_TRACE=0; shift;;
    --record-trace) RECORD_TRACE=1; shift;;
    --capacity) CAPACITY=$2; shift 2;;
    --button-every) BUTTON_EVERY=$2; shift 2;;
    --producer-cpu) PRODUCER_CPU=$2; shift 2;;
    --consumer-cpu) CONSUMER_CPU=$2; shift 2;;
    --ingress-cpu) INGRESS_CPU=$2; shift 2;;
    --stall-after-ms) STALL_AFTER_MS=$2; shift 2;;
    --stall-ms) STALL_MS=$2; shift 2;;
    --scenario) SCENARIO=$2; shift 2;;
    --rate-label) RATE_LABEL=$2; shift 2;;
    --perf) PERF_MODE=$2; shift 2;;
    --sudo-bridge) SUDO_BRIDGE=1; shift;;
    --verbose) QUIET=0; shift;;
    --) shift; break;;
    *) echo "unknown option: $1" >&2; exit 2;;
  esac
done
[[ -n "$OUT" && -n "$POLICY" ]] || { echo "--out and --policy are required" >&2; exit 2; }
if [[ -n "$SYNTHETIC_RATE" && -n "$DEVICE" ]] || [[ -z "$SYNTHETIC_RATE" && -z "$DEVICE" ]]; then
  echo "choose exactly one of --synthetic-rate or --device" >&2; exit 2
fi
mkdir -p "$OUT/client"
rm -f "$OUT"/{bridge.env,bridge.stderr,bridge.stdout,client.stderr,client.stdout,integrity.json,MANIFEST.sha256,producer-trace.csv}
SOCKET_BASE=${XDG_RUNTIME_DIR:-/tmp}
mkdir -p "$SOCKET_BASE"
SOCKET="$SOCKET_BASE/hfior-hfior-${$}-${RANDOM}.sock"
cleanup() {
  set +e
  [[ -n "${BRIDGE_PID:-}" ]] && kill "$BRIDGE_PID" 2>/dev/null
  rm -f "$SOCKET"
}
trap cleanup EXIT INT TERM

cat /proc/stat > "$OUT/proc-stat.before" 2>/dev/null || true
cat /proc/interrupts > "$OUT/interrupts.before" 2>/dev/null || true

bridge=("$BIN_DIR/hfior-bridge" --socket "$SOCKET" --capacity "$CAPACITY" --duration 0 --stats "$OUT/bridge.env" --synthetic-timestamps "$TIMESTAMP_MODE")
if [[ -n "$SYNTHETIC_RATE" ]]; then bridge+=(--synthetic-rate "$SYNTHETIC_RATE" --button-every "$BUTTON_EVERY"); else bridge+=(--device "$DEVICE"); fi
(( PRODUCER_TRACE )) && bridge+=(--trace "$OUT/producer-trace.csv")
(( QUIET )) && bridge+=(--quiet)
if [[ -n "$PRODUCER_CPU" ]] && command -v taskset >/dev/null; then bridge=(taskset -c "$PRODUCER_CPU" "${bridge[@]}"); fi
if (( SUDO_BRIDGE )); then bridge=(sudo -E -- "${bridge[@]}"); fi
"${bridge[@]}" >"$OUT/bridge.stdout" 2>"$OUT/bridge.stderr" &
BRIDGE_PID=$!
for _ in $(seq 1 1000); do
  [[ -S "$SOCKET" ]] && break
  kill -0 "$BRIDGE_PID" 2>/dev/null || { cat "$OUT/bridge.stderr" >&2; exit 1; }
  sleep 0.002
done
[[ -S "$SOCKET" ]] || { echo "bridge socket did not appear" >&2; exit 1; }

client=("$BIN_DIR/hfior-latency-client" --socket "$SOCKET" --output-dir "$OUT/client" --policy "$POLICY" --duration "$DURATION" --frame-hz "$FRAME_HZ" --input-phase "$INPUT_PHASE" --base-work-us "$BASE_WORK_US" --integration-work-us "$INTEGRATION_WORK_US" --callback-work-ns "$CALLBACK_WORK_NS" --ack-placement "$ACK_PLACEMENT" --spin-us "$SPIN_US" --tail-threshold-us "$TAIL_THRESHOLD_US")
[[ -n "$CHECKS" ]] && client+=(--checks-per-frame "$CHECKS")
[[ -n "$FINAL_RECHECKS" ]] && client+=(--final-rechecks "$FINAL_RECHECKS")
[[ -n "$CONSUMER_CPU" ]] && client+=(--consumer-cpu "$CONSUMER_CPU")
[[ -n "$INGRESS_CPU" ]] && client+=(--ingress-cpu "$INGRESS_CPU")
(( STALL_MS > 0 )) && client+=(--stall-after-ms "$STALL_AFTER_MS" --stall-ms "$STALL_MS")
(( RECORD_TRACE )) && client+=(--record-trace)
(( QUIET )) && client+=(--quiet)

CLIENT_RC=0
if [[ "$PERF_MODE" == stat ]] && command -v perf >/dev/null; then
  perf stat -x, -o "$OUT/perf-stat.csv" -- "${client[@]}" >"$OUT/client.stdout" 2>"$OUT/client.stderr" || CLIENT_RC=$?
else
  "${client[@]}" >"$OUT/client.stdout" 2>"$OUT/client.stderr" || CLIENT_RC=$?
fi
wait "$BRIDGE_PID" || BRIDGE_RC=$?
BRIDGE_RC=${BRIDGE_RC:-0}
BRIDGE_PID=
cat /proc/stat > "$OUT/proc-stat.after" 2>/dev/null || true
cat /proc/interrupts > "$OUT/interrupts.after" 2>/dev/null || true

cat > "$OUT/run.env" <<ENV
schema=3
policy=$POLICY
source=$([[ -n "$SYNTHETIC_RATE" ]] && echo synthetic || echo physical)
synthetic_rate_hz=${SYNTHETIC_RATE:-0}
device=${DEVICE:-}
rate_label=${RATE_LABEL:-${SYNTHETIC_RATE:-physical}}
scenario=$SCENARIO
duration_s=$DURATION
frame_hz=$FRAME_HZ
input_phase=$INPUT_PHASE
base_work_us=$BASE_WORK_US
integration_work_us=$INTEGRATION_WORK_US
callback_work_ns=$CALLBACK_WORK_NS
ack_placement=$ACK_PLACEMENT
checks_per_frame=${CHECKS:-auto}
final_rechecks=${FINAL_RECHECKS:-auto}
spin_us=$SPIN_US
timestamp_mode=$TIMESTAMP_MODE
producer_trace=$PRODUCER_TRACE
record_trace=$RECORD_TRACE
capacity=$CAPACITY
button_every=$BUTTON_EVERY
producer_cpu=${PRODUCER_CPU:-unbound}
consumer_cpu=${CONSUMER_CPU:-unbound}
ingress_cpu=${INGRESS_CPU:-unbound}
sudo_bridge=$SUDO_BRIDGE
stall_after_ms=$STALL_AFTER_MS
stall_ms=$STALL_MS
client_rc=$CLIENT_RC
bridge_rc=$BRIDGE_RC
ENV

python3 - "$OUT" <<'PY'
import json, pathlib, sys
out=pathlib.Path(sys.argv[1])
errors=[]
def env(path):
    d={}
    if path.exists():
        for line in path.read_text(errors='replace').splitlines():
            if '=' in line:
                k,v=line.split('=',1); d[k]=v
    return d
try: summary=json.loads((out/'client'/'summary.json').read_text())
except Exception as e: summary={}; errors.append(f'summary:{e}')
b=env(out/'bridge.env'); r=env(out/'run.env')
policy=summary.get('policy',r.get('policy',''))
published=int(b.get('records_published','-1'))
actual=int(summary.get('eager_records',0) if policy=='eager-thread' else summary.get('ring_records',0))
shadow=int(summary.get('shadow_ring_records',0))
checks={
 'client_rc':int(r.get('client_rc','1'))==0,
 'bridge_rc':int(r.get('bridge_rc','1'))==0,
 'summary_present':bool(summary),
 'bridge_stats_present':bool(b),
 'producer_ring_drops':int(summary.get('producer_ring_drops',-1))==0,
 'producer_eager_drops':int(summary.get('producer_eager_drops',-1))==0,
 'sequence_gaps':int(summary.get('sequence_gaps',-1))==0,
 'duplicate_or_reordered':int(summary.get('duplicate_or_reordered',-1))==0,
 'bridge_invalid_acknowledgements':int(b.get('invalid_acknowledgements','-1'))==0,
 'ring_depth_final':int(b.get('ring_depth','-1'))==0,
 'path_records_match_published': actual==published,
}
if policy=='eager-thread':
    # The shadow cursor is initialized after the start-barrier reply. A few
    # records can legitimately publish in that barrier window while the eager
    # socket path still receives them. The selected eager path, not the
    # diagnostic shadow, is the integrity source of truth.
    checks['shadow_records_do_not_exceed_published']=0 <= shadow <= published
valid=all(checks.values()) and not errors
wall=float(b.get('wall_s','0') or 0)
result={
 'schema':3,'valid':valid,'errors':errors,'checks':checks,'policy':policy,
 'evidence_class':summary.get('evidence_class'),
 'records_published':published,'actual_path_records':actual,'shadow_ring_records':shadow,
 'syn_reports':int(b.get('syn_reports','0')),
 'observed_reports_per_s':int(b.get('syn_reports','0'))/wall if wall else 0,
 'ring_depth_final':int(b.get('ring_depth','-1')),
 'shadow_record_deficit':published-shadow if policy=='eager-thread' else 0,
 'acknowledgements':int(b.get('acknowledgements','0')),
}
(out/'integrity.json').write_text(json.dumps(result,indent=2)+'\n')
if not valid:
    print(json.dumps(result,indent=2),file=sys.stderr)
    raise SystemExit(1)
PY

(
  cd "$OUT"
  find . -type f ! -name MANIFEST.sha256 -print0 | sort -z | xargs -0 sha256sum > MANIFEST.sha256
)
trap - EXIT INT TERM
rm -f "$SOCKET"
