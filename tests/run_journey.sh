#!/bin/bash
# run_journey.sh <script.qcs> [outdir] [extra q-continuum args...]
# Runs a journey script in a sandbox: its own config and workspace (never the user's), launches of ParaView,
# terminals and editors recorded instead of opened, a hard timeout, and only the sandbox's own processes reaped.
set -u
here=$(cd "$(dirname "$0")" && pwd)
bin=${QC_BIN:-$here/../build/q-continuum}
script=$1; shift
out=${1:-$(mktemp -d)}; shift || true
sb=$out/sandbox
# QC_KEEP_SANDBOX=1: a second launch on the sandbox an earlier one left (an app restart); QC_NO_REAP=1 leaves its
# processes running for that next launch (failure injection: a killed app's orphaned ranks); QC_APP_ENV="K=v ..." goes
# to the app (and what it starts) only, not to this script's own copies (failure injection: the disk-full shim)
[ -n "${QC_KEEP_SANDBOX:-}" ] || rm -rf "$sb"
mkdir -p "$sb/cfg/q-continuum" "$sb/ws" "$sb/home"
printf 'workspace=%s\nprocesses=%s\nui_scale=1.0\nstyle=%s\nhistory=1\nnotify=0\nreview_save=0\n' "$sb/ws" "${QC_PROCS:-4}" "${QC_THEME:-0}" > "$sb/cfg/q-continuum/settings.ini"
# "#settings: key=value" lines in the script go into settings.ini ($SB expanded): state left by an earlier session
grep '^#settings: ' "$script" | sed 's/^#settings: //' | SB=$sb envsubst '$SB' >> "$sb/cfg/q-continuum/settings.ini"
[ -z "${QC_KEEP_SANDBOX:-}" ] && [ -n "${QC_SEED_CASES:-}" ] && for c in $QC_SEED_CASES; do cp -a "$c" "$sb/ws/"; done
export OUT=$out LAUNCH=$out/launches.txt SB=$sb
envsubst '$OUT $LAUNCH $SB' < "$script" > "$out/script${QC_PHASE:-}.qcs"
SB=$sb XDG_CONFIG_HOME=$sb/cfg XDG_CACHE_HOME=$sb/cache QC_TEST_DEME_HOME=$sb/home QC_TEST_LAUNCH=$out/launches.txt QC_TEST_TERMINAL=$out/terminal.txt QC_VIDEOS=$out/videos \
    timeout ${QC_TIMEOUT:-600} env ${QC_APP_ENV:-} "$bin" --hidden --script "$out/script${QC_PHASE:-}.qcs" "$@" > "$out/journey${QC_PHASE:-}.log" 2>&1
rc=$?
# reap anything still running inside the sandbox (mpirun ranks of an interrupted run)
[ -z "${QC_NO_REAP:-}" ] && for p in /proc/[0-9]*; do
    cwd=$(readlink "$p/cwd" 2>/dev/null) || continue
    case "$cwd" in "$sb"*) kill -TERM "${p#/proc/}" 2>/dev/null;; esac
done
grep -E "SUMMARY|FAIL" "$out/journey${QC_PHASE:-}.log"
echo "rc=$rc  log=$out/journey${QC_PHASE:-}.log"
exit $rc
