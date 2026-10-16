#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
PROJECT_DIR=$(dirname "$SCRIPT_DIR")
BIN="$PROJECT_DIR/sws"
LUA_REPORT="$SCRIPT_DIR/wrk_report.lua"

MODELS="fork thread event"
CONNECTIONS="10 50 100 500 1000 5000 10000"
DURATION=30
WARMUP=5
WRK_THREADS=$(nproc)
REPETITIONS=3
PORT=8080
URL_PATH="/index.html"
DOC_ROOT="$PROJECT_DIR/www"
KEEPALIVE=5
BACKLOG=4096
COOLDOWN=5
SAMPLE_INTERVAL=1
SERVER_CPUS=""
CLIENT_CPUS=""
OUT_DIR="$PROJECT_DIR/results/$(date +%Y%m%d-%H%M%S)"

usage() {
  cat <<EOF
Usage: $0 [options]

  -m "<models>"       models to test (default: "$MODELS")
  -c "<list>"         concurrent connections (default: "$CONNECTIONS")
  -d <seconds>        duration of each measured run (default: $DURATION)
  -w <seconds>        warm-up before each run, 0 disables (default: $WARMUP)
  -t <threads>        wrk threads (default: nproc = $WRK_THREADS)
  -r <n>              repetitions of each scenario (default: $REPETITIONS)
  -p <port>           server port (default: $PORT)
  -f <path>           requested URL path (default: $URL_PATH)
  -R <dir>            document root (default: $DOC_ROOT)
  -k <seconds>        server keep-alive timeout (default: $KEEPALIVE)
  -b <n>              server listen backlog (default: $BACKLOG)
  -i <seconds>        resource sampling interval (default: $SAMPLE_INTERVAL)
  -S <cpu list>       pin the server to these CPUs (taskset), e.g. 0-3
  -C <cpu list>       pin wrk to these CPUs (taskset), e.g. 4-7
  -o <dir>            output directory (default: results/<timestamp>)
  -h                  show this help

Example (server on cores 0-3, wrk on cores 4-7):
  $0 -S 0-3 -C 4-7 -t 4 -c "100 1000 10000" -d 60 -r 5
EOF
}

while getopts "m:c:d:w:t:r:p:f:R:k:b:i:S:C:o:h" option; do
  case "$option" in
    m) MODELS=$OPTARG ;;
    c) CONNECTIONS=$OPTARG ;;
    d) DURATION=$OPTARG ;;
    w) WARMUP=$OPTARG ;;
    t) WRK_THREADS=$OPTARG ;;
    r) REPETITIONS=$OPTARG ;;
    p) PORT=$OPTARG ;;
    f) URL_PATH=$OPTARG ;;
    R) DOC_ROOT=$OPTARG ;;
    k) KEEPALIVE=$OPTARG ;;
    b) BACKLOG=$OPTARG ;;
    i) SAMPLE_INTERVAL=$OPTARG ;;
    S) SERVER_CPUS=$OPTARG ;;
    C) CLIENT_CPUS=$OPTARG ;;
    o) OUT_DIR=$OPTARG ;;
    h) usage; exit 0 ;;
    *) usage >&2; exit 1 ;;
  esac
done

for tool in wrk curl make pgrep; do
  if ! command -v "$tool" >/dev/null 2>&1; then
    echo "error: '$tool' not found (Ubuntu: sudo apt install wrk curl build-essential procps)" >&2
    exit 1
  fi
done
if [[ -n "$SERVER_CPUS$CLIENT_CPUS" ]] && ! command -v taskset >/dev/null; then
  echo "error: taskset not found (sudo apt install util-linux)" >&2
  exit 1
fi

ulimit -n "$(ulimit -Hn)" 2>/dev/null || true

SERVER_PREFIX=()
CLIENT_PREFIX=()
[[ -n "$SERVER_CPUS" ]] && SERVER_PREFIX=(taskset -c "$SERVER_CPUS")
[[ -n "$CLIENT_CPUS" ]] && CLIENT_PREFIX=(taskset -c "$CLIENT_CPUS")

CLK_TCK=$(getconf CLK_TCK)
URL="http://127.0.0.1:${PORT}${URL_PATH}"
SERVER_PID=""
SAMPLER_PID=""

mkdir -p "$OUT_DIR/raw"
RESULTS="$OUT_DIR/results.csv"
SUMMARY="$OUT_DIR/summary.csv"

cleanup() {
  [[ -n "$SAMPLER_PID" ]] && kill "$SAMPLER_PID" 2>/dev/null || true
  stop_server
}
trap cleanup EXIT
trap 'exit 130' INT TERM

record_environment() {
  {
    echo "date: $(date -Is)"
    echo "kernel: $(uname -srvmo)"
    [[ -r /etc/os-release ]] && grep PRETTY_NAME /etc/os-release
    echo "cpu: $(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2- | sed 's/^ //')"
    echo "cpus (nproc): $(nproc)"
    echo "memory: $(grep MemTotal /proc/meminfo | awk '{print $2 " kB"}')"
    echo "wrk: $(wrk -v 2>&1 | head -n1)"
    echo "sws: $("$BIN" --version)"
    echo "ulimit -n: $(ulimit -n)"
    for key in net.core.somaxconn net.ipv4.tcp_max_syn_backlog \
               net.ipv4.ip_local_port_range net.ipv4.tcp_tw_reuse \
               net.ipv4.tcp_fin_timeout kernel.pid_max kernel.threads-max; do
      echo "$key: $(sysctl -n "$key" 2>/dev/null | tr '\t' ' ')"
    done
    echo
    echo "models: $MODELS"
    echo "connections: $CONNECTIONS"
    echo "duration: ${DURATION}s, warm-up: ${WARMUP}s, repetitions: $REPETITIONS"
    echo "wrk threads: $WRK_THREADS (max: connections)"
    echo "url: $URL ($(stat -c %s "$DOC_ROOT$URL_PATH" 2>/dev/null || echo '?') bytes)"
    echo "server keep-alive: ${KEEPALIVE}s, backlog: $BACKLOG"
    echo "server cpus: ${SERVER_CPUS:-any}, client cpus: ${CLIENT_CPUS:-any}"
  } > "$OUT_DIR/env.txt"
}

start_server() {
  local model=$1 log=$2

  "${SERVER_PREFIX[@]}" "$BIN" --model "$model" --port "$PORT" \
    --root "$DOC_ROOT" --keepalive "$KEEPALIVE" --backlog "$BACKLOG" \
    > "$log" 2>&1 &
  SERVER_PID=$!

  for _ in $(seq 50); do
    if curl -s -o /dev/null "$URL"; then
      return 0
    fi
    sleep 0.1
  done
  echo "error: server ($model) did not start, see $log" >&2
  exit 1
}

stop_server() {
  if [[ -n "$SERVER_PID" ]] && kill -0 "$SERVER_PID" 2>/dev/null; then
    pkill -TERM -P "$SERVER_PID" 2>/dev/null || true
    kill -TERM "$SERVER_PID" 2>/dev/null || true
    wait "$SERVER_PID" 2>/dev/null || true
  fi
  SERVER_PID=""
}

cpu_ticks() {
  awk '{ print $14 + $15 + $16 + $17 }' "/proc/$1/stat"
}

sample_resources() {
  local pid=$1 output=$2

  while kill -0 "$pid" 2>/dev/null; do
    local pids files=() count=0
    pids="$pid $(pgrep -P "$pid" | tr '\n' ' ' || true)"
    for p in $pids; do
      files+=("/proc/$p/status" "/proc/$p/smaps_rollup")
      count=$((count + 1))
    done
    cat "${files[@]}" 2>/dev/null | awk -v procs="$count" '
      /^VmRSS:/   { rss += $2 }
      /^Pss:/     { pss += $2 }
      /^Threads:/ { threads += $2 }
      END { printf "%d %d %d %d\n", rss, pss, threads, procs }' >> "$output"
    sleep "$SAMPLE_INTERVAL"
  done
}

run_wrk() {
  local connections=$1 duration=$2 output=$3
  local threads=$WRK_THREADS

  (( threads > connections )) && threads=$connections
  "${CLIENT_PREFIX[@]}" wrk -t"$threads" -c"$connections" -d"${duration}s" \
    --timeout 10s --latency -s "$LUA_REPORT" "$URL" > "$output" 2>&1 || true
}

run_scenario() {
  local model=$1 connections=$2 repetition=$3
  local tag="${model}-c${connections}-r${repetition}"
  local samples="$OUT_DIR/raw/$tag.samples"
  local start_ticks end_ticks start_time end_time wrk_line resources cpu

  echo "==> model=$model connections=$connections repetition=$repetition"
  start_server "$model" "$OUT_DIR/raw/$tag.server.log"

  if (( WARMUP > 0 )); then
    run_wrk "$connections" "$WARMUP" "$OUT_DIR/raw/$tag.warmup.txt"
  fi

  : > "$samples"
  start_ticks=$(cpu_ticks "$SERVER_PID")
  start_time=$(date +%s.%N)
  sample_resources "$SERVER_PID" "$samples" &
  SAMPLER_PID=$!

  run_wrk "$connections" "$DURATION" "$OUT_DIR/raw/$tag.wrk.txt"

  end_time=$(date +%s.%N)
  kill "$SAMPLER_PID" 2>/dev/null || true
  wait "$SAMPLER_PID" 2>/dev/null || true
  SAMPLER_PID=""
  sleep 1
  end_ticks=$(cpu_ticks "$SERVER_PID")
  stop_server

  cpu=$(awk -v t0="$start_ticks" -v t1="$end_ticks" -v hz="$CLK_TCK" \
            -v s0="$start_time" -v s1="$end_time" \
            'BEGIN { printf "%.1f", (t1 - t0) / hz / (s1 - s0) * 100 }')
  resources=$(awk '
      $1 > rss { rss = $1 } $2 > pss { pss = $2 }
      $3 > thr { thr = $3 } $4 > prc { prc = $4 }
      { sum_rss += $1; sum_pss += $2; n++ }
      END { if (n == 0) n = 1
            printf "%d,%d,%d,%d,%d,%d", rss, sum_rss / n, pss, sum_pss / n, thr, prc }' "$samples")

  wrk_line=$(grep '^WRK_CSV,' "$OUT_DIR/raw/$tag.wrk.txt" | cut -d, -f2- || true)
  if [[ -z "$wrk_line" ]]; then
    echo "    wrk produced no result, see $OUT_DIR/raw/$tag.wrk.txt" >&2
    wrk_line="0,0,0,0,0,0,0,0,0,0,0,0,0,0,0"
  fi

  echo "$model,$connections,$repetition,$wrk_line,$cpu,$resources" >> "$RESULTS"
  echo "    $(echo "$wrk_line" | cut -d, -f3) req/s, p99 $(echo "$wrk_line" | cut -d, -f8) ms, cpu ${cpu}%"
  sleep "$COOLDOWN"
}

summarize() {
  awk -F, '
    NR == 1 { next }
    {
      key = $1 "," $2
      if (!(key in n)) order[++keys] = key
      n[key]++
      rps[key] += $6;  rps2[key] += $6 * $6
      lat[key] += $7;  p50[key] += $9; p99[key] += $11
      cpu[key] += $19; pss[key] += $22; rss[key] += $20
      thr[key] += $24; prc[key] += $25
      err[key] += $14 + $15 + $16 + $17 + $18
    }
    END {
      print "model,connections,repetitions,rps_mean,rps_stdev,latency_mean_ms,latency_p50_ms,latency_p99_ms,cpu_percent,rss_peak_kb,pss_peak_kb,threads_peak,processes_peak,errors_total"
      for (i = 1; i <= keys; i++) {
        k = order[i]; c = n[k]
        mean = rps[k] / c
        var = rps2[k] / c - mean * mean; if (var < 0) var = 0
        printf "%s,%d,%.2f,%.2f,%.3f,%.3f,%.3f,%.1f,%d,%d,%d,%d,%d\n", k, c,
               mean, sqrt(var), lat[k] / c, p50[k] / c, p99[k] / c,
               cpu[k] / c, rss[k] / c, pss[k] / c, thr[k] / c, prc[k] / c,
               err[k]
      }
    }' "$RESULTS" > "$SUMMARY"
}

echo "building..."
make -C "$PROJECT_DIR" >/dev/null
record_environment

echo "model,connections,repetition,requests,bytes,rps,latency_mean_ms,latency_stdev_ms,latency_p50_ms,latency_p90_ms,latency_p99_ms,latency_p999_ms,latency_max_ms,errors_connect,errors_read,errors_write,errors_status,errors_timeout,cpu_percent,rss_peak_kb,rss_mean_kb,pss_peak_kb,pss_mean_kb,threads_peak,processes_peak" > "$RESULTS"

for model in $MODELS; do
  for connections in $CONNECTIONS; do
    for repetition in $(seq "$REPETITIONS"); do
      run_scenario "$model" "$connections" "$repetition"
    done
  done
done

summarize
echo
echo "results: $RESULTS"
echo "summary: $SUMMARY"
column -s, -t < "$SUMMARY" 2>/dev/null || cat "$SUMMARY"
