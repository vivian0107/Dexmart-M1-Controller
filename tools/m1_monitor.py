#!/usr/bin/env python3
"""Read-only M1 diagnostic serial monitor; emits JSON for an AGX health adapter.

Serial mode requires pyserial. Parser, --stdin replay and tests use the stdlib.
It never sends motor/parameter commands; loss of serial progress expires health.
"""
import argparse
import json
import re
import sys
import time
import uuid


class CommandSession:
    """One correlated request at a time; a serial write is never an ACK."""
    def __init__(self, nodes=(1, 2), session_id=None):
        self.queue = ["INFO", f"NODES {nodes[0]} {nodes[1]}", "WATCH ON", "PROBE"]
        self.session_id = session_id or uuid.uuid4().hex[:12]
        self.sequence = 0
        self.pending = None
        self.error = None
        self.verified = False
        self.probe_result = None

    def next_line(self, now):
        if self.pending and now >= self.pending["deadline"]:
            self.error = f"ack_timeout_{self.pending['verb'].lower()}"
            self.verified = False
            self.pending = None
            self.queue.clear()
        if self.pending or self.error or not self.queue:
            return None
        command = self.queue.pop(0)
        self.sequence += 1
        request_id = f"{self.session_id}-{self.sequence}"
        self.pending = dict(id=request_id, verb=command.split()[0], command=command,
                            deadline=now + 1.5, accepted=False)
        return f"REQ {request_id} {command}\n"

    def accept(self, line, now):
        if not self.pending or not line.startswith("M1D1 ACK ") or len(line) > 256:
            return False
        try:
            fields = {}
            for item in line.strip().split()[2:]:
                key, value = item.split("=", 1)
                if key in fields:
                    return False
                fields[key] = value
            if set(fields) != {"id", "cmd", "status", "reason"}:
                return False
            if fields["id"] != self.pending["id"] or fields["cmd"] != self.pending["verb"]:
                return False
            if now >= self.pending["deadline"]:
                self.next_line(now)  # Expire it before considering a late ACK.
                return False
            status, reason = fields["status"], fields["reason"]
            if not re.fullmatch(r"[a-z_]+", reason):
                return False
            if status == "accepted":
                if self.pending["verb"] != "PROBE" or reason != "pending" or self.pending["accepted"]:
                    return False
                self.pending["accepted"] = True
                self.pending["deadline"] = now + 30
                return True
            if status not in {"complete", "failed", "rejected", "cancelled"}:
                return False
            if status != "complete":
                self.error = f"{self.pending['verb'].lower()}_{status}_{reason}"
                self.verified = False
                self.queue.clear()
            elif self.pending["verb"] == "PROBE":
                if not self.pending["accepted"] or reason not in {"ok", "partial", "profile_mismatch"}:
                    return False
                self.probe_result = reason
            elif reason != "ok":
                return False
            elif self.pending["command"] == "WATCH ON":
                self.verified = True
            self.pending = None
            return True
        except ValueError:
            return False


class HealthMonitor:
    def __init__(self, nodes=(1, 2), stale_after=1.5):
        if len(nodes) != 2 or len(set(nodes)) != 2 or any(not 1 <= n <= 127 for n in nodes):
            raise ValueError("two distinct CAN node IDs from 1 to 127 are required")
        if not 0 < stale_after <= 10:
            raise ValueError("invalid telemetry expiry")
        self.nodes = tuple(nodes)
        self.stale_after = stale_after
        self.samples = {}
        self.disconnected = False
        self.command_error = None
        self.probe_result = None

    def accept(self, line, now):
        if line.startswith("M1D1 BOOT "):
            self.samples.clear()
            return True
        if len(line) > 1024 or not line.startswith("M1D1 HEALTH "):
            return False
        try:
            fields = {}
            for part in line.strip().split()[2:]:
                key, value = part.split("=", 1)
                if key in fields:
                    return False
                fields[key] = value
            node = int(fields["node"])
            seq = int(fields["seq"])
            uptime = int(fields["uptime_ms"])
            health, link = fields["health"], fields["link"]
            ready = fields["drive_ready"]
            if node not in self.nodes or seq <= 0 or not 0 <= uptime <= 0xFFFFFFFF:
                return False
            if health not in {"healthy", "impaired", "unhealthy", "unknown"}:
                return False
            if link not in {"online", "offline", "unknown"} or ready not in {"0", "1"}:
                return False
            if fields["motion_allowed"] != "0" or not re.fullmatch(r"[a-z_]+", fields["reason"]):
                return False
            sw_text, er_text, mode_text = fields["status"], fields["error"], fields["mode"]
            if sw_text != "NA" and not re.fullmatch(r"0x[0-9a-fA-F]{4}", sw_text):
                return False
            if er_text != "NA" and not re.fullmatch(r"0x[0-9a-fA-F]{2}", er_text):
                return False
            sw = None if sw_text == "NA" else int(sw_text, 16)
            er = None if er_text == "NA" else int(er_text, 16)
            mode = None if mode_text == "NA" else int(mode_text)
            if mode is not None and not -128 <= mode <= 127:
                return False
            if health == "healthy" and (link != "online" or sw is None or er != 0 or sw & 8):
                return False
            if fields.get("mapping", "unknown") not in {"unknown", "match", "mismatch"}:
                return False
            if health == "healthy" and fields.get("mapping") == "mismatch":
                return False
            if ready == "1" and (health != "healthy" or mode != 3 or sw is None or
                                  sw & 0x006F != 0x0027 or not sw & 0x0200):
                return False
            previous = self.samples.get(node)
            if previous:
                if seq <= previous["seq"]:
                    return False  # A repeated sample cannot renew its lease.
                if (uptime - previous["uptime_ms"]) & 0xFFFFFFFF >= 0x80000000:
                    self.samples.clear()  # Reboot without seeing BOOT: discard old evidence.
                    return False
            self.samples[node] = dict(fields, node=node, seq=seq, uptime_ms=uptime,
                                      drive_ready=ready == "1", motion_allowed=False,
                                      received_at=now)
            self.disconnected = False
            return True
        except (KeyError, ValueError):
            return False

    def snapshot(self, now):
        nodes = []
        for node in self.nodes:
            sample = self.samples.get(node)
            if self.disconnected or sample is None or now - sample["received_at"] > self.stale_after:
                reason = "serial_disconnected" if self.disconnected else "not_sampled" if sample is None else "telemetry_stale"
                nodes.append(dict(node=node, health="unknown", link="unknown", reason=reason,
                                  drive_ready=False, motion_allowed=False))
            else:
                item = {k: v for k, v in sample.items() if k != "received_at"}
                item["receiver_age_ms"] = round((now - sample["received_at"]) * 1000)
                nodes.append(item)
        levels = {n["health"] for n in nodes}
        health = ("unhealthy" if "unhealthy" in levels else "unknown" if "unknown" in levels
                  else "impaired" if "impaired" in levels else "healthy")
        if self.command_error and health != "unhealthy":
            health = "unknown"
        evidence = {"healthy": "Healthy", "impaired": "Impaired", "unhealthy": "Unhealthy", "unknown": "Unknown"}[health]
        return dict(schema="m1-diagnostics-health/v1", health=health, evidence_level=evidence,
                    driver_status="normal" if health == "healthy" else "error",
                    drive_ready=all(n["drive_ready"] for n in nodes), motion_allowed=False,
                    command_error=self.command_error, probe_result=self.probe_result, nodes=nodes)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--port", help="ST-LINK VCP, for example COM7 or /dev/ttyACM0")
    source.add_argument("--stdin", action="store_true", help="replay diagnostic lines from stdin")
    parser.add_argument("--nodes", nargs=2, type=int, default=[1, 2], metavar=("LEFT", "RIGHT"))
    parser.add_argument("--raw", action="store_true", help="also log raw diagnostic lines to stderr")
    args = parser.parse_args()
    monitor = HealthMonitor(args.nodes)

    def publish():
        print(json.dumps(monitor.snapshot(time.monotonic()), ensure_ascii=True), flush=True)

    if args.stdin:
        for line in sys.stdin:
            if monitor.accept(line, time.monotonic()):
                publish()
        return 0
    try:
        import serial
    except ImportError:
        parser.error("serial mode requires: python -m pip install pyserial")
    try:
        with serial.Serial(args.port, 115200, timeout=0.1, write_timeout=1) as port:
            # Wait through startup printing before sending console commands.
            configure_at = time.monotonic() + 0.3
            session = None
            monitor.command_error = "handshake_pending"
            published_at = 0.0
            buffer = bytearray()
            discarding = False
            while True:
                now = time.monotonic()
                if configure_at is not None and now >= configure_at:
                    session = CommandSession(args.nodes)
                    configure_at = None
                if session is not None:
                    request = session.next_line(now)
                    if request is not None:
                        port.write(request.encode("ascii"))
                    monitor.command_error = session.error or (None if session.verified else "handshake_pending")
                for byte in port.read(256):
                    if byte in (10, 13):
                        if not discarding and buffer:
                            line = buffer.decode("ascii", errors="replace")
                            if args.raw:
                                print(line, file=sys.stderr, flush=True)
                            monitor.accept(line, time.monotonic())
                            if session is not None:
                                session.accept(line, time.monotonic())
                                monitor.command_error = session.error or (None if session.verified else "handshake_pending")
                                monitor.probe_result = session.probe_result
                            if line.startswith("M1D1 BOOT "):
                                configure_at = time.monotonic() + 0.3
                                session = None
                                monitor.command_error = "handshake_pending"
                                monitor.probe_result = None
                        buffer.clear()
                        discarding = False
                    elif not discarding:
                        if len(buffer) >= 1024:
                            buffer.clear()
                            discarding = True
                        else:
                            buffer.append(byte)
                if time.monotonic() - published_at >= 0.5:
                    publish()
                    published_at = time.monotonic()
    except KeyboardInterrupt:
        return 0
    except (serial.SerialException, OSError) as error:
        monitor.disconnected = True
        publish()
        print(str(error), file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
