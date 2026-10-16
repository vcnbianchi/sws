#!/usr/bin/env bash

set -uo pipefail

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
PROJECT_DIR=$(dirname "$SCRIPT_DIR")
PORT=${1:-4545}
MODELS=${2:-"fork thread event"}
HOST=127.0.0.1
BASE="http://$HOST:$PORT"

BIN="$PROJECT_DIR/sws"
[[ -x "$BIN.exe" ]] && BIN="$BIN.exe"
if [[ ! -x "$BIN" ]]; then
  echo "error: $BIN not found, run 'make' first" >&2
  exit 1
fi

DOC_ROOT=$(mktemp -d)
LOG=$(mktemp)
SERVER_PID=""
PASSED=0
FAILED=0

cp "$PROJECT_DIR/www/index.html" "$DOC_ROOT/index.html"
mkdir -p "$DOC_ROOT/docs"
printf 'docs index\n' > "$DOC_ROOT/docs/index.html"
printf 'plain text\n' > "$DOC_ROOT/notes.txt"
head -c 1048576 /dev/urandom > "$DOC_ROOT/big.bin"

cleanup() {
  stop_server
  rm -rf "$DOC_ROOT" "$LOG"
}
trap cleanup EXIT

start_server() {
  "$BIN" --model "$1" --port "$PORT" --root "$DOC_ROOT" --keepalive 2 \
    > "$LOG" 2>&1 &
  SERVER_PID=$!
  for _ in $(seq 50); do
    curl -s -o /dev/null "$BASE/" && return 0
    sleep 0.1
  done
  echo "server did not start:" >&2
  cat "$LOG" >&2
  exit 1
}

stop_server() {
  if [[ -n "$SERVER_PID" ]]; then
    kill "$SERVER_PID" 2>/dev/null
    wait "$SERVER_PID" 2>/dev/null
    SERVER_PID=""
  fi
}

check() {
  local description=$1 expected=$2 actual=$3
  if [[ "$actual" == "$expected" ]]; then
    PASSED=$((PASSED + 1))
    printf '  ok    %s\n' "$description"
  else
    FAILED=$((FAILED + 1))
    printf '  FAIL  %s (expected "%s", got "%s")\n' "$description" \
      "$expected" "$actual"
  fi
}

status_of() {
  curl -s -o /dev/null -w '%{http_code}' "$@"
}

raw_request() {
  local payload=$1
  (
    exec 3<>"/dev/tcp/$HOST/$PORT" || exit 1
    printf '%b' "$payload" >&3
    timeout 5 cat <&3 | tr -d '\r' | grep -a '^HTTP/1.1 ' | cut -d' ' -f2 \
      | tr '\n' ' ' | sed 's/ $//'
  )
}

file_size() {
  wc -c < "$1" | tr -d ' '
}

run_tests() {
  local model=$1 size length body pids failures=0

  check "GET / returns 200" 200 "$(status_of "$BASE/")"
  body=$(curl -s "$BASE/")
  check "GET / serves index.html" "$(cat "$DOC_ROOT/index.html")" "$body"
  check "Content-Type of .txt" "text/plain; charset=utf-8" \
    "$(curl -s -o /dev/null -w '%{content_type}' "$BASE/notes.txt")"

  size=$(file_size "$DOC_ROOT/big.bin")
  check "1 MiB binary file is complete" "$size" \
    "$(curl -s "$BASE/big.bin" | wc -c | tr -d ' ')"
  check "1 MiB binary file is identical" "ok" \
    "$(curl -s "$BASE/big.bin" | cmp -s - "$DOC_ROOT/big.bin" && echo ok)"

  length=$(curl -s -I "$BASE/index.html" | tr -d '\r' \
    | awk -F': ' 'tolower($1) == "content-length" { print $2 }')
  check "HEAD reports Content-Length" "$(file_size "$DOC_ROOT/index.html")" \
    "$length"
  check "HEAD has no body" 0 \
    "$(curl -s --head "$BASE/index.html" -o /dev/null -w '%{size_download}')"

  check "missing file returns 404" 404 "$(status_of "$BASE/nothing.html")"
  check "POST returns 405" 405 "$(status_of -X POST -d x "$BASE/")"
  check "405 lists allowed methods" "Allow: GET, HEAD" \
    "$(curl -s -i -X DELETE "$BASE/" | tr -d '\r' | grep '^Allow:')"
  check "directory without slash redirects" 301 "$(status_of "$BASE/docs")"
  check "directory index" "docs index" "$(curl -s "$BASE/docs/")"
  check "path traversal is rejected" 403 \
    "$(status_of --path-as-is "$BASE/../../etc/passwd")"
  check "encoded traversal is rejected" 403 \
    "$(status_of --path-as-is "$BASE/%2e%2e/%2e%2e/etc/passwd")"
  check "query string is ignored" 200 "$(status_of "$BASE/notes.txt?a=1")"

  check "keep-alive reuses the connection" 1 \
    "$(curl -s -v "$BASE/" "$BASE/notes.txt" -o /dev/null -o /dev/null 2>&1 \
       | grep -ci 're-using existing')"
  check "HTTP/1.0 closes by default" "Connection: close" \
    "$(curl -s -0 -i "$BASE/" | tr -d '\r' | grep '^Connection:')"

  check "malformed request returns 400" 400 \
    "$(raw_request 'NONSENSE\r\n\r\n')"
  check "HTTP/1.1 without Host returns 400" 400 \
    "$(raw_request 'GET / HTTP/1.1\r\n\r\n')"
  check "unsupported version returns 505" 505 \
    "$(raw_request 'GET / HTTP/2.0\r\nHost: x\r\n\r\n')"
  check "pipelined requests are answered in order" "200 404 200" \
    "$(raw_request 'GET / HTTP/1.1\r\nHost: x\r\n\r\nGET /no HTTP/1.1\r\nHost: x\r\n\r\nGET /notes.txt HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n')"
  check "oversized header returns 431" 431 \
    "$(raw_request "GET / HTTP/1.1\r\nHost: x\r\nX-Big: $(head -c 9000 /dev/zero | tr '\0' 'a')\r\n\r\n")"

  pids=()
  for i in $(seq 50); do
    curl -s -o /dev/null -w '%{http_code}\n' "$BASE/notes.txt?i=$i" \
      > "$LOG.$i" &
    pids+=($!)
  done
  for pid in "${pids[@]}"; do wait "$pid"; done
  for i in $(seq 50); do
    [[ "$(cat "$LOG.$i")" == 200 ]] || failures=$((failures + 1))
    rm -f "$LOG.$i"
  done
  check "50 concurrent clients" 0 "$failures"

  check "server still alive" "alive" \
    "$(kill -0 "$SERVER_PID" 2>/dev/null && echo alive)"
}

for model in $MODELS; do
  echo "model: $model"
  start_server "$model"
  run_tests "$model"
  stop_server
done

echo
echo "passed: $PASSED, failed: $FAILED"
[[ $FAILED -eq 0 ]]
