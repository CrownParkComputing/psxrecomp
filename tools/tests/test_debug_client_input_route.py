#!/usr/bin/env python3
"""Hermetic tests for debug_client.py's deterministic input-route runner."""

import importlib.util
import json
import tempfile
import unittest
from pathlib import Path
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "debug_client", ROOT / "debug_client.py")
DEBUG = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(DEBUG)


class FakeSocket:
    def __init__(self):
        self.timeout = 10.0
        self.timeout_history = []

    def gettimeout(self):
        return self.timeout

    def settimeout(self, timeout):
        self.timeout_history.append(timeout)
        self.timeout = timeout


class OneShotSocket(FakeSocket):
    def __init__(self, peer=("127.0.0.1", DEBUG.NATIVE_PORT)):
        super().__init__()
        self.peer = peer
        self.closed = False

    def getpeername(self):
        return self.peer

    def close(self):
        self.closed = True


class AdvancingClock:
    def __init__(self, step=0.01):
        self.value = 0.0
        self.step = step

    def __call__(self):
        value = self.value
        self.value += self.step
        return value


def write_route(directory, document):
    path = Path(directory) / "route.json"
    path.write_text(json.dumps(document), encoding="utf-8")
    return path


class InputRouteTests(unittest.TestCase):
    def test_transport_reconnects_for_one_request_server_contract(self):
        initial = OneShotSocket()
        fresh = OneShotSocket()
        commands = []

        def fake_send(sock, command):
            commands.append((sock, command["cmd"]))
            return {"ok": True}

        with mock.patch.object(DEBUG, "send_cmd", side_effect=fake_send), \
                mock.patch.object(DEBUG, "connect", return_value=fresh) as reconnect:
            transport = DEBUG._InputRouteTransport(initial)
            transport.request({"cmd": "input_route_clear"})
            transport.request({"cmd": "input_route_append"})

        self.assertEqual(commands, [
            (initial, "input_route_clear"),
            (fresh, "input_route_append"),
        ])
        reconnect.assert_called_once_with(
            "127.0.0.1", DEBUG.NATIVE_PORT, timeout=10.0)
        self.assertTrue(fresh.closed)

    def test_digest_normalizes_integer_spellings_and_ignores_metadata(self):
        with tempfile.TemporaryDirectory() as directory:
            route_a = write_route(directory, {
                "format": DEBUG.INPUT_ROUTE_FORMAT,
                "description": "same semantic route",
                "steps": [
                    {"pad_word": "0xffff", "frames": 2},
                    {"pad_word": 65534, "frames": "3"},
                ],
            })
            route_b = Path(directory) / "route-b.json"
            route_b.write_text(
                '{"steps":[{"frames":2,"pad_word":65535},'
                '{"frames":3,"pad_word":"0xFFFE"}],'
                '"format":"psxrecomp-input-route-v1",'
                '"description":"different metadata"}',
                encoding="utf-8")

            loaded_a = DEBUG.load_input_route(route_a)
            loaded_b = DEBUG.load_input_route(route_b)

        self.assertEqual(loaded_a["steps"], [
            {"buttons": 0xFFFF, "frames": 2},
            {"buttons": 0xFFFE, "frames": 3},
        ])
        self.assertEqual(loaded_a["frame_count"], 5)
        self.assertEqual(loaded_a["digest"], loaded_b["digest"])
        self.assertTrue(loaded_a["digest"].startswith("sha256:"))

    def test_rejects_non_16_bit_words_and_non_positive_durations(self):
        with tempfile.TemporaryDirectory() as directory:
            bad_word = write_route(directory, {
                "format": DEBUG.INPUT_ROUTE_FORMAT,
                "steps": [{"pad_word": "0x10000", "frames": 1}],
            })
            with self.assertRaisesRegex(DEBUG.InputRouteError, "pad_word"):
                DEBUG.load_input_route(bad_word)

            bad_frames = write_route(directory, {
                "format": DEBUG.INPUT_ROUTE_FORMAT,
                "steps": [{"pad_word": "0xFFFF", "frames": 0}],
            })
            with self.assertRaisesRegex(DEBUG.InputRouteError, "frames"):
                DEBUG.load_input_route(bad_frames)

    def test_uploads_in_order_and_requires_exact_completion(self):
        route_document = {
            "format": DEBUG.INPUT_ROUTE_FORMAT,
            "steps": [
                {"pad_word": "0xFFFF", "frames": 2},
                {"pad_word": "0xBFFF", "frames": 1},
            ],
        }
        socket = FakeSocket()
        calls = []
        statuses = iter([
            {"ok": True, "active": True, "steps": 2,
             "index": 0, "remaining": 1},
            {"ok": True, "active": False, "steps": 2,
             "index": 2, "remaining": 0},
        ])

        def send(_socket, command):
            calls.append(command)
            name = command["cmd"]
            if name == "input_route_clear":
                return {"ok": True}
            if name == "input_route_append":
                return {"ok": True, "steps": len([
                    call for call in calls
                    if call["cmd"] == "input_route_append"])}
            if name == "input_route_start":
                return {"ok": True, "steps": 2, "start_frame": 100}
            if name == "input_route_status":
                return next(statuses)
            if name == "frame":
                return {"ok": True, "frame": 103}
            raise AssertionError(name)

        with tempfile.TemporaryDirectory() as directory:
            route_path = write_route(directory, route_document)
            clock = AdvancingClock()
            with mock.patch.object(DEBUG, "send_cmd", side_effect=send), \
                    mock.patch.object(DEBUG.time, "sleep",
                                      side_effect=AssertionError("sleep")):
                receipt = DEBUG.run_input_route(
                    socket, route_path, timeout=1.0, clock=clock)

        self.assertEqual([call["cmd"] for call in calls], [
            "input_route_clear",
            "input_route_append",
            "input_route_append",
            "input_route_start",
            "input_route_status",
            "input_route_status",
            "frame",
        ])
        self.assertEqual(calls[1]["buttons"], 0xFFFF)
        self.assertEqual(calls[2]["buttons"], 0xBFFF)
        self.assertEqual(receipt["completion"], "exact")
        self.assertEqual(receipt["start"]["frame"], 100)
        self.assertEqual(receipt["end"]["frame"], 103)
        self.assertEqual(receipt["end"]["status"]["index"], 2)
        self.assertEqual(socket.timeout, 10.0)

    def test_timeout_stops_route_and_does_not_claim_completion(self):
        socket = FakeSocket()
        calls = []

        def send(_socket, command):
            calls.append(command)
            name = command["cmd"]
            if name == "input_route_clear":
                return {"ok": True}
            if name == "input_route_append":
                return {"ok": True, "steps": 1}
            if name == "input_route_start":
                return {"ok": True, "steps": 1, "start_frame": 7}
            if name == "input_route_status":
                return {"ok": True, "active": True, "steps": 1,
                        "index": 0, "remaining": 1}
            if name == "input_route_stop":
                return {"ok": True}
            raise AssertionError(name)

        with tempfile.TemporaryDirectory() as directory:
            route_path = write_route(directory, {
                "format": DEBUG.INPUT_ROUTE_FORMAT,
                "steps": [{"pad_word": "0xFFFF", "frames": 1}],
            })
            with mock.patch.object(DEBUG, "send_cmd", side_effect=send):
                with self.assertRaises(DEBUG.InputRouteTimeout):
                    DEBUG.run_input_route(
                        socket, route_path, timeout=0.05,
                        clock=AdvancingClock(step=0.02))

        self.assertEqual(calls[-1]["cmd"], "input_route_stop")
        self.assertNotIn("frame", [call["cmd"] for call in calls])

    def test_cli_command_emits_one_compact_receipt_line(self):
        receipt = {
            "ok": True,
            "completion": "exact",
            "route_digest": "sha256:abc",
            "start": {"frame": 10},
            "end": {"frame": 12},
        }
        with mock.patch.object(DEBUG, "run_input_route",
                               return_value=receipt):
            output = DEBUG.run_command(
                FakeSocket(), ["input_route", "route.json"],
                route_timeout=2.0)

        self.assertEqual(json.loads(output), receipt)
        self.assertNotIn("\n", output)
        self.assertNotIn(": ", output)


if __name__ == "__main__":
    unittest.main()
