#!/bin/sh
# codesign-debugger.sh <binary> — ad-hoc self-sign <binary> with the entitlements
# task_for_pid needs, so the macOS out-of-process Mach stepper's live legs
# (asmtest_mach_trace_call/_trace_attached/_run_to) can attach to their own forked
# tracees. See docs/internal/implementations/macos-oop-mach-stepper.md's
# Constraints & gates.
#
#   com.apple.security.cs.debugger   authorizes task_for_pid of a target carrying
#                                     get-task-allow (below is that target too, since
#                                     the tracer forks and traces its own tracee)
#   com.apple.security.get-task-allow lets the tracer obtain the forked child's own
#                                     task port
#
# codesign -s - (ad-hoc, no real signing identity) needs no Apple Developer account.
# The FIRST task_for_pid call after signing may trigger a one-time admin-authorization
# dialog granting a ~10-hour session; `sudo make mach-stepper-test` reaches unentitled
# non-SIP targets directly instead, skipping this script entirely.
#
# Host-only tooling (Xcode Command Line Tools' codesign) — no Docker lane, since
# macOS cannot run in this project's Linux containers (CLAUDE.md's "add the missing
# dependency to a Dockerfile" rule does not apply to a missing OS).
set -eu

prog=$(basename "$0")
bin="${1:?usage: $prog <binary>}"

plist=$(mktemp -t asmtest-mach-entitlements)
trap 'rm -f "$plist"' EXIT

cat > "$plist" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>com.apple.security.cs.debugger</key>
	<true/>
	<key>com.apple.security.get-task-allow</key>
	<true/>
</dict>
</plist>
EOF

codesign --entitlements "$plist" -f -s - "$bin"
