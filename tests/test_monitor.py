import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location("m1_monitor", Path(__file__).resolve().parents[1] / "tools/m1_monitor.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


def sample(node=1, seq=1, uptime=500, health="healthy", status="0x0221", error="0x00", ready=0):
    return (f"M1D1 HEALTH seq={seq} node={node} uptime_ms={uptime} link=online health={health} "
            f"reason=not_enabled status={status} error={error} mode=3 drive_ready={ready} motion_allowed=0")


class MonitorTests(unittest.TestCase):
    def test_no_data_and_only_one_drive_never_normal(self):
        m = module.HealthMonitor()
        self.assertEqual(m.snapshot(0)["driver_status"], "error")
        self.assertTrue(m.accept(sample(), 0))
        self.assertEqual(m.snapshot(0)["evidence_level"], "Unknown")
        self.assertTrue(m.accept(sample(node=2), 0))
        s = m.snapshot(0)
        self.assertEqual(s["driver_status"], "normal")
        self.assertFalse(s["drive_ready"])
        self.assertFalse(s["motion_allowed"])

    def test_fault_warning_and_disconnection(self):
        m = module.HealthMonitor()
        m.accept(sample(), 0)
        m.accept(sample(node=2, health="unhealthy", error="0x01"), 0)
        self.assertEqual(m.snapshot(0)["evidence_level"], "Unhealthy")
        m.accept(sample(node=2, seq=2, health="impaired", status="0x02A1"), 0.1)
        self.assertEqual(m.snapshot(0.1)["evidence_level"], "Impaired")
        m.disconnected = True
        self.assertEqual(m.snapshot(0.2)["health"], "unknown")

    def test_expiry_duplicate_and_reboot(self):
        m = module.HealthMonitor()
        m.accept(sample(), 0)
        m.accept(sample(node=2), 0)
        self.assertFalse(m.accept(sample(), 1))
        self.assertEqual(m.snapshot(2)["health"], "unknown")
        m.accept("M1D1 BOOT firmware=m1-diag-1", 2)
        self.assertEqual(m.samples, {})
        m.accept(sample(seq=10, uptime=10000), 2)
        self.assertFalse(m.accept(sample(seq=11, uptime=1), 3))
        self.assertEqual(m.samples, {})

    def test_invalid_data_cannot_claim_normal_or_ready(self):
        m = module.HealthMonitor()
        for line in (sample(error="0x01"), sample(status="0x0228"), sample(ready=1),
                     sample().replace("motion_allowed=0", "motion_allowed=1"),
                     sample().replace("seq=1", "seq=-1"), sample() + " node=1",
                     sample().replace("health=healthy", "health=fine"), "garbage"):
            self.assertFalse(m.accept(line, 0), line)
        self.assertEqual(m.snapshot(0)["health"], "unknown")
        self.assertTrue(m.accept(sample(status="0x0227", ready=1), 0))

    def test_unrelated_node_and_wrapped_uptime(self):
        m = module.HealthMonitor()
        self.assertFalse(m.accept(sample(node=3), 0))
        self.assertTrue(m.accept(sample(uptime=0xFFFFFF00), 0))
        self.assertTrue(m.accept(sample(seq=2, uptime=10), 0.3))

    def test_missing_command_ack_cannot_show_normal(self):
        m = module.HealthMonitor()
        m.accept(sample(), 0)
        m.accept(sample(node=2), 0)
        m.command_error = "ack_timeout_watch"
        self.assertEqual(m.snapshot(0)["driver_status"], "error")
        self.assertEqual(m.snapshot(0)["health"], "unknown")


class CommandSessionTests(unittest.TestCase):
    def reply(self, session, status="complete", reason="ok"):
        p = session.pending
        return f"M1D1 ACK id={p['id']} cmd={p['verb']} status={status} reason={reason}"

    def test_ordered_configuration_requires_matching_acks(self):
        s = module.CommandSession(session_id="s")
        for expected in ["INFO", "NODES 1 2", "WATCH ON"]:
            self.assertEqual(s.next_line(0), f"REQ s-{s.sequence} {expected}\n")
            self.assertIsNone(s.next_line(0.1))
            self.assertTrue(s.accept(self.reply(s), 0.2))
        self.assertTrue(s.verified)
        self.assertEqual(s.next_line(0.3), "REQ s-4 PROBE\n")
        self.assertTrue(s.accept(self.reply(s, "accepted", "pending"), 0.4))
        self.assertTrue(s.accept(self.reply(s, reason="partial"), 1))
        self.assertEqual(s.probe_result, "partial")

    def test_wrong_token_command_and_late_ack_do_not_complete(self):
        s = module.CommandSession(session_id="a")
        s.next_line(0)
        correct = self.reply(s)
        self.assertFalse(s.accept(correct.replace("id=a-1", "id=old"), 0.2))
        self.assertFalse(s.accept(correct.replace("cmd=INFO", "cmd=STATUS"), 0.2))
        self.assertFalse(s.accept(correct, 2))
        self.assertEqual(s.error, "ack_timeout_info")
        self.assertFalse(s.verified)

    def test_accepted_is_not_complete_and_duplicates_do_not_extend_deadline(self):
        s = module.CommandSession(session_id="p")
        s.queue = ["PROBE"]
        s.next_line(0)
        self.assertFalse(s.accept(self.reply(s), 0.1))
        self.assertTrue(s.accept(self.reply(s, "accepted", "pending"), 0.2))
        deadline = s.pending["deadline"]
        self.assertFalse(s.accept(self.reply(s, "accepted", "pending"), 20))
        self.assertEqual(s.pending["deadline"], deadline)
        self.assertIsNone(s.next_line(31))
        self.assertEqual(s.error, "ack_timeout_probe")

    def test_rejection_and_cancellation_fail_explicitly(self):
        for status in ("failed", "rejected", "cancelled"):
            s = module.CommandSession(session_id="r")
            s.next_line(0)
            self.assertTrue(s.accept(self.reply(s, status, "read_only"), 0.1))
            self.assertIsNotNone(s.error)
            self.assertFalse(s.verified)
            self.assertIsNone(s.next_line(0.2))


if __name__ == "__main__":
    unittest.main()
