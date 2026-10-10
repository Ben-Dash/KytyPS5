#!/usr/bin/env bash
# Builds the replay worktree under G:\dev\_locks\build.lock: waits while another thread holds it,
# takes it atomically (noclobber), and releases only its own lock.
lock=/g/dev/_locks/build.lock
log=${1:-build_replay.log}
until ( set -o noclobber; echo "dispatch-replay $(date +%H:%M:%S)" > "$lock" ) 2>/dev/null; do sleep 15; done
cmd //c "G:\dev\KytyPS5-replay\build_replay.bat" > "/g/dev/KytyPS5-replay/$log" 2>&1
grep -n "error:" "/g/dev/KytyPS5-replay/$log" | head -20
tail -n 1 "/g/dev/KytyPS5-replay/$log"
grep -q "^dispatch-replay" "$lock" && mv "$lock" "/g/dev/_locks/build.lock.done_replay_$(date +%H%M%S)"
