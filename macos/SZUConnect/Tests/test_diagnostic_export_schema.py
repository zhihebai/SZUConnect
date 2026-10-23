#!/usr/bin/env python3
"""Validate anonymous synthetic macOS report fixtures; never read app data.

Requires jsonschema 4.x. Optional --report validates an explicitly supplied test
export in addition to the isolated positive and negative fixtures below.
"""
import argparse
import copy
import datetime
import json
import math
import re
from pathlib import Path
import unittest

from jsonschema import Draft202012Validator, FormatChecker
from jsonschema.exceptions import ValidationError


ROOT = Path(__file__).resolve().parents[3]
SCHEMA = json.loads((ROOT / "shared/schemas/macos-diagnostics-export.schema.json").read_text())
Draft202012Validator.check_schema(SCHEMA)
FORMATS = FormatChecker()


@FORMATS.checks("date-time")
def valid_timestamp(value):
    # Keep format checks active even when jsonschema's optional RFC3339 extra
    # is not installed. Swift's ISO8601 encoder always includes a timezone.
    if not isinstance(value, str):
        return True
    if not re.fullmatch(r"\d{4}-\d{2}-\d{2}[Tt]\d{2}:\d{2}:\d{2}(\.\d+)?([Zz]|[+-]([01]\d|2[0-3]):[0-5]\d)", value):
        return False
    try:
        datetime.datetime.fromisoformat(value.upper().replace("Z", "+00:00"))
        return True
    except ValueError:
        return False


VALIDATOR = Draft202012Validator(SCHEMA, format_checker=FORMATS)
STAMP = "2026-10-23T00:00:00Z"
IDENTIFIER = "00000000-0000-4000-8000-000000000001"


def environment():
    return {"connection": "ethernet", "systemProxy": "off", "tun": "detected", "refreshing": False}


def recovery():
    return {"state": "paused", "readOnly": False, "paused": True, "networkAvailable": True,
            "networkRemembered": True, "sessionTrusted": False, "holdReasons": ["userPaused"],
            "cooldownRemainingSeconds": 30}


def physical():
    return {"physicalInterfaceCount": 2, "serviceCount": 2, "candidateCount": 2,
            "rejections": {"missingDNS": 1}}


def sample_context():
    return {"phase": "connectivityCheck", "operation": "automatic", "monitoringMode": "recover",
            "area": "automatic", "startedAt": STAMP, "environment": environment()}


def sample():
    return {"id": IDENTIFIER, "date": STAMP, "online": True, "appleState": "success",
            "microsoftState": "timeout", "interface": "有线", "probeMode": "complete",
            "latencyMilliseconds": 125, "appleMilliseconds": 125, "microsoftMilliseconds": 120000,
            "websiteTotal": 12, "websitePassed": 11, "context": sample_context(), "completeResult": "partial"}


def failure_context():
    return {"phase": "operationResult", "kind": "connection", "result": "operationFailed",
            "startedAt": STAMP, "collectedAt": STAMP, "operation": "manual", "monitoringMode": "recover",
            "area": "teaching", "environment": environment(), "status": "uncertain",
            "campusVerification": {"state": "unconfirmed", "issue": "dnsTls"}, "recovery": recovery(),
            "physicalNetwork": physical(), "physicalCollectedAt": STAMP}


def event():
    return {"id": IDENTIFIER, "date": STAMP, "message": "匿名故障记录", "kind": "failure", "context": failure_context()}


def legacy_report():
    return {"formatVersion": 1, "exportedAt": STAMP, "appVersion": "1.0.0", "build": "33",
            "mode": "recover", "summary": "匿名测试摘要", "measurement": "检测响应时间", "samples": [],
            "campusVerification": {"state": "notChecked", "issue": "none"}}


def modern_report():
    result = legacy_report()
    result.update({"samples": [sample()], "events": [event()], "environment": environment(),
                   "recovery": recovery(), "physicalNetwork": physical(), "physicalCollectedAt": STAMP,
                   "settings": {"monitoringMode": "recover", "area": "automatic", "probeMode": "complete",
                                "websiteStrategy": "direct", "intervalSeconds": 30},
                   "crashes": {"status": "notCollected", "lookbackDays": 7,
                               "reason": "unsupportedMetadataSource", "records": []}})
    return result


def crash_record():
    return {"date": STAMP, "version": "1.0.0", "exceptionCode": "0x0000000B", "moduleOffset": "0x0000000000000010"}


def authentication():
    return {"area": "teaching", "stage": "submission", "path": "/cgi-bin/srun_portal", "httpStatus": 200,
            "acID": "12", "result": "uncertain", "code": "conflictingStatus", "serverCode": "unknown", "saved": True}


def assert_wire_valid(value):
    # Non-finite numbers are not JSON values. Reject them before schema checks,
    # just as JSONEncoder does for the actual Swift report.
    wire = json.dumps(value, allow_nan=False)
    VALIDATOR.validate(json.loads(wire))


class MacDiagnosticSchemaTests(unittest.TestCase):
    def valid(self, value):
        assert_wire_valid(value)

    def invalid(self, value):
        try:
            assert_wire_valid(value)
        except (ValueError, ValidationError):
            # Validator exceptions and wire-encoding errors both mean the
            # report cannot be a valid document on the distribution boundary.
            return
        self.fail("Fixture unexpectedly validates")

    def mutate(self, path, value):
        result = modern_report()
        container = result
        for component in path[:-1]:
            container = container[component]
        container[path[-1]] = value
        return result

    def test_legacy_and_optional_contexts(self):
        self.valid(legacy_report())
        older = legacy_report()
        older["samples"] = [{"id": IDENTIFIER, "date": STAMP, "online": False, "appleState": "timeout",
                              "microsoftState": "network", "interface": "未知接口"}]
        self.valid(older)
        self.valid(modern_report())
        unconfigured_interval = modern_report(); del unconfigured_interval["settings"]["intervalSeconds"]
        self.valid(unconfigured_interval)
        for key in ["environment", "recovery", "settings", "events", "physicalCollectedAt", "crashes"]:
            with self.subTest(omitted=key):
                value = modern_report(); del value[key]; self.valid(value)

    def test_all_known_enums(self):
        for name in ["wifi", "ethernet", "multiple", "tunnelHidden", "none", "unknown"]:
            with self.subTest(connection=name): self.valid(self.mutate(["environment", "connection"], name))
        for name in ["on", "off", "unknown"]:
            with self.subTest(proxy=name): self.valid(self.mutate(["environment", "systemProxy"], name))
        for name in ["detected", "notDetected", "unknown"]:
            with self.subTest(tun=name): self.valid(self.mutate(["environment", "tun"], name))
        for name in ["off", "needsAccount", "paused", "needsTrust", "coolingDown", "limited", "ready", "awaitingProof", "unknown"]:
            with self.subTest(recovery=name): self.valid(self.mutate(["recovery", "state"], name))
        for name in ["manual", "observe", "recover"]:
            with self.subTest(mode=name): self.valid(self.mutate(["settings", "monitoringMode"], name))
        for name in ["automatic", "teaching", "dormitory"]:
            with self.subTest(area=name): self.valid(self.mutate(["settings", "area"], name))
        for name in ["basicPassed", "allPassed", "partial", "unavailable", "cancelled"]:
            with self.subTest(result=name): self.valid(self.mutate(["samples", 0, "completeResult"], name))

    def test_anonymous_context_rejects_unknown_or_private_fields(self):
        cases = [(["environment", "connection"], "en0"), (["environment", "systemProxy"], "http://proxy.invalid"),
                 (["environment", "tun"], "utun9"), (["recovery", "state"], "arbitrary error"),
                 (["recovery", "holdReasons"], ["userPaused", "userPaused"]),
                 (["recovery", "holdReasons"], ["unknown"]),
                 (["settings", "area"], "12"), (["settings", "websiteStrategy"], "proxy URL"),
                 (["samples", 0, "context", "phase"], "other"),
                 (["samples", 0, "context", "operation"], "interactive"),
                 (["events", 0, "context", "result"], "server raw text"),
                 (["events", 0, "context", "status"], "HTTP 401 response"),
                 (["events", 0, "context", "campusVerification", "issue"], "raw exception"),
                 (["events", 0, "context", "physicalNetwork", "rejections"], {"rawInterface": 1})]
        for path, value in cases:
            with self.subTest(path=path): self.invalid(self.mutate(path, value))
        for context_path in [["environment"], ["samples", 0, "context"], ["events", 0, "context"]]:
            with self.subTest(private=context_path):
                value = modern_report(); container = value
                for key in context_path: container = container[key]
                container["SSID"] = "FAKE-NETWORK"; self.invalid(value)

    def test_counts_and_durations_are_bounded(self):
        paths = [("recovery", "cooldownRemainingSeconds", 1, 86400),
                 ("settings", "intervalSeconds", 0, 86400),
                 ("samples", 0, "appleMilliseconds", 0, 120000),
                 ("samples", 0, "microsoftMilliseconds", 0, 120000),
                 ("samples", 0, "websiteTotal", 0, 12),
                 ("physicalNetwork", "candidateCount", 0, 4096)]
        for *path, low, high in paths:
            with self.subTest(path=path):
                self.valid(self.mutate(path, low)); self.valid(self.mutate(path, high))
                self.invalid(self.mutate(path, low - 1)); self.invalid(self.mutate(path, high + 1))
                self.invalid(self.mutate(path, math.inf)); self.invalid(self.mutate(path, math.nan))
        self.invalid(self.mutate(["recovery", "cooldownRemainingSeconds"], 1.5))
        self.invalid(self.mutate(["events", 0, "context", "physicalNetwork", "rejections", "missingDNS"], 0))
        self.invalid(self.mutate(["samples"], [sample()] * 361))
        self.invalid(self.mutate(["events"], [event()] * 301))
        value = modern_report(); value["events"] = [{"id": IDENTIFIER, "date": STAMP, "message": "x" * 8193}]
        self.invalid(value)

    def test_dates_and_uuids(self):
        self.invalid(self.mutate(["exportedAt"], "not a date"))
        self.invalid(self.mutate(["samples", 0, "date"], 123456))
        self.invalid(self.mutate(["samples", 0, "context", "startedAt"], "2026-99-99"))
        self.invalid(self.mutate(["events", 0, "id"], "not a UUID"))
        self.invalid(self.mutate(["events", 0, "context", "physicalCollectedAt"], "today"))
        self.invalid(self.mutate(["physicalCollectedAt"], 0))

    def test_old_events_and_failure_pairing(self):
        value = modern_report(); value["events"] = [{"id": IDENTIFIER, "date": STAMP, "message": "匿名旧记录"}]
        self.valid(value)
        value["events"][0]["kind"] = "failure"; self.invalid(value)
        value = modern_report(); del value["events"][0]["kind"]; self.invalid(value)
        value = modern_report(); del value["events"][0]["context"]; self.invalid(value)

    def test_typed_authentication_preserves_bounded_protocol_facts(self):
        value = modern_report(); value["events"][0]["authentication"] = authentication()
        self.valid(value)
        without_failure = copy.deepcopy(value)
        del without_failure["events"][0]["context"]; del without_failure["events"][0]["kind"]
        self.valid(without_failure)
        for stage, path in [("homepage", "/srun_portal_pc"), ("challenge", "/cgi-bin/get_challenge")]:
            good = copy.deepcopy(value); good["events"][0]["authentication"].update({"stage": stage, "path": path})
            self.valid(good)
        dormitory = copy.deepcopy(value)
        dormitory["events"][0]["authentication"].update({"area": "dormitory", "path": "/eportal/portal/login"})
        for field in ["acID", "httpStatus", "serverCode"]:
            del dormitory["events"][0]["authentication"][field]
        self.valid(dormitory)
        for field, bad in [("area", "automatic"), ("stage", "unreviewed"), ("path", "https://example.invalid/?secret=fake"),
                           ("httpStatus", 99), ("httpStatus", 600), ("acID", "0"), ("acID", "012"),
                           ("acID", "123456789"), ("acID", "192.0.2.1"), ("result", "raw server message"),
                           ("code", "raw error"), ("serverCode", "raw text"), ("saved", "true"),
                           ("stage", "homepage"), ("area", "dormitory")]:
            with self.subTest(field=field, bad=bad):
                bad_value = copy.deepcopy(value); bad_value["events"][0]["authentication"][field] = bad
                self.invalid(bad_value)
        bad_value = copy.deepcopy(value); bad_value["events"][0]["authentication"]["clientIP"] = "192.0.2.1"
        self.invalid(bad_value)

    def test_conservative_crash_status_and_synthetic_metadata(self):
        self.valid(modern_report())
        self.valid(self.mutate(["crashes", "reason"], "readOnlyOrPreview"))
        for status in ["found", "none", "accessDenied", "unavailable", "cancelled", "partial"]:
            with self.subTest(status=status):
                value = modern_report(); value["crashes"] = {"status": status, "lookbackDays": 7,
                                                            "collectedAt": STAMP, "records": [crash_record()] if status in ["found", "partial"] else []}
                self.valid(value)
        self.invalid(self.mutate(["crashes", "status"], "neverCrashed"))
        self.invalid(self.mutate(["crashes", "reason"], "read all logs"))
        self.invalid(self.mutate(["crashes", "lookbackDays"], 8))
        self.invalid(self.mutate(["crashes", "records"], [crash_record()]))
        found = modern_report(); found["crashes"] = {"status": "found", "lookbackDays": 7, "records": [crash_record()]}
        self.valid(found)
        found["crashes"]["records"] = [crash_record()] * 6; self.invalid(found)
        for field, bad in [("version", "arbitrary"), ("exceptionCode", "SIGSEGV raw content"),
                           ("exceptionCode", "0x000000ab"), ("moduleOffset", "0x0000000010000001")]:
            with self.subTest(field=field):
                value = copy.deepcopy(found); value["crashes"]["records"] = [crash_record()]
                value["crashes"]["records"][0][field] = bad; self.invalid(value)
        value = copy.deepcopy(found); value["crashes"]["records"] = [crash_record()]
        value["crashes"]["records"][0]["executablePath"] = "/Fake/Test.app"; self.invalid(value)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--report", type=Path)
    arguments, remaining = parser.parse_known_args()
    if arguments.report:
        raw = arguments.report.read_text()
        value = json.loads(raw, parse_constant=lambda token: (_ for _ in ()).throw(ValueError(token)))
        assert_wire_valid(value)
        print("Explicit test report validates against the macOS schema")
    unittest.main(argv=[__file__, *remaining])
