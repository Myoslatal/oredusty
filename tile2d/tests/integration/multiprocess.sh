#!/usr/bin/env bash
# Tile2D - end to end across three processes: a dedicated server (no graphics at all) plus two bots
# that join over KCP/UDP, play scripted input and verify that their simulation stays in sync.
set -euo pipefail

build_dir="${1:?usage: multiprocess.sh <build-dir>}"
server="$build_dir/apps/server/tile2d_server"
bot="$build_dir/apps/bot/tile2d_bot"
[[ -x "$server" ]] || { echo "missing $server"; exit 1; }
[[ -x "$bot" ]] || { echo "missing $bot"; exit 1; }

port=$(( 20000 + RANDOM % 20000 ))
log_dir="$(mktemp -d)"
cleanup() { kill "$server_pid" 2>/dev/null || true; rm -rf "$log_dir"; }
trap cleanup EXIT

echo "==> dedicated server on udp:$port"
"$server" --port "$port" --max-players 4 --snapshot-rate 2 --tick-rate 240 --quiet >"$log_dir/server.log" 2>&1 &
server_pid=$!
sleep 0.5
kill -0 "$server_pid" 2>/dev/null || { echo "server exited early"; cat "$log_dir/server.log"; exit 1; }

status=0
for seed in 1 2; do
    echo "==> bot $seed joins"
    if "$bot" --connect "127.0.0.1:$port" --name "bot$seed" --seed "$seed" --ticks 240 --quiet \
            >"$log_dir/bot$seed.log" 2>&1; then
        cat "$log_dir/bot$seed.log"
    else
        echo "bot $seed FAILED"; cat "$log_dir/bot$seed.log"; status=1
    fi
done

kill -TERM "$server_pid" 2>/dev/null || true
wait "$server_pid" 2>/dev/null || true
echo "==> server summary"
grep -E '^\[server\]' "$log_dir/server.log" | tail -1 || cat "$log_dir/server.log"

[[ $status -eq 0 ]] || exit $status
grep -qE 'commands=[1-9]' "$log_dir/server.log" || { echo "the server never received a command"; exit 1; }
echo "==> multiprocess integration passed"
