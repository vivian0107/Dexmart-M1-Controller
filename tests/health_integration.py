"""Exercise the actual C emitter -> Python parser -> C2 two-state mapping."""
import importlib.util
from pathlib import Path
import subprocess
import sys

spec = importlib.util.spec_from_file_location("m1_monitor", Path(__file__).resolve().parents[1] / "tools/m1_monitor.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
output = subprocess.run([sys.argv[1], "--emit-health"], check=True, text=True, capture_output=True).stdout
monitor = module.HealthMonitor()
snapshots = []
for line in output.splitlines():
    if monitor.accept(line, 0):
        snapshots.append(monitor.snapshot(0))
assert any(s["driver_status"] == "normal" for s in snapshots), output
assert any(any(n["reason"] == "drive_fault" for n in s["nodes"]) for s in snapshots), output
assert snapshots[-1]["evidence_level"] == "Unhealthy", snapshots[-1]
assert snapshots[-1]["driver_status"] == "error", snapshots[-1]
assert all(not s["motion_allowed"] for s in snapshots)
print("C diagnostic output -> AGX health parser -> C2 status mapping passed")

output = subprocess.run([sys.argv[1], "--emit-ack"], check=True, text=True, capture_output=True).stdout
session = module.CommandSession(session_id="link")
assert session.next_line(0) == "REQ link-1 INFO\n"
for line in output.splitlines():
    if session.accept(line, 0.1) and session.pending is None:
        session.next_line(0.1)
assert session.pending is None and not session.queue and session.error is None, output
assert session.verified and session.probe_result == "ok", output
print("C request ACKs -> AGX correlated session verification passed")
