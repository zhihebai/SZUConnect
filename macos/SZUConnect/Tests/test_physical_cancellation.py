#!/usr/bin/env python3
"""Cancel real loopback transfers without changing networking or submitting credentials."""
import argparse
import concurrent.futures
import ctypes
import http.server
import json
from pathlib import Path
import subprocess
import tempfile
import threading
import time
import unittest


class PhysicalCancellationTests(unittest.TestCase):
    baseline = False
    results = []

    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(prefix="szu-curl-cancel-")
        library = Path(cls.temporary.name) / "fixture.dylib"
        source = Path(__file__).resolve().parents[1] / "Sources/CampusPhysicalCurl.cpp"
        subprocess.run(["xcrun", "clang++", "-std=c++17", "-dynamiclib",
                        "-DSZU_CAMPUS_CURL_TESTING", str(source), "-lcurl", "-o", str(library)], check=True)
        cls.library = ctypes.CDLL(str(library))
        cls.library.szu_campus_cancellation_create.restype = ctypes.c_void_p
        for name in ["szu_campus_cancellation_cancel", "szu_campus_cancellation_release"]:
            getattr(cls.library, name).argtypes = [ctypes.c_void_p]
        cls.library.szu_campus_test_cancel_loopback.argtypes = [ctypes.c_uint, ctypes.c_void_p]
        cls.library.szu_campus_test_cancel_loopback.restype = ctypes.c_int

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    def measure(self, body_started):
        reached = threading.Event()
        release = threading.Event()

        class Handler(http.server.BaseHTTPRequestHandler):
            def do_GET(self):
                if body_started:
                    self.send_response(200)
                    self.send_header("Content-Length", "16")
                    self.end_headers()
                    self.wfile.write(b"test")
                    self.wfile.flush()
                reached.set()
                release.wait(3)
                if not body_started:
                    try:
                        self.send_response(200)
                        self.send_header("Content-Length", "0")
                        self.end_headers()
                    except (BrokenPipeError, ConnectionResetError):
                        pass

            def log_message(self, *_):
                pass

        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        server.daemon_threads = True
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        token = self.library.szu_campus_cancellation_create()
        self.assertTrue(token)
        try:
            with concurrent.futures.ThreadPoolExecutor(max_workers=1) as executor:
                future = executor.submit(self.library.szu_campus_test_cancel_loopback, server.server_port, token)
                self.assertTrue(reached.wait(2), "The isolated request reaches the selected slow stage")
                # Avoid cancelling inside the initial progress callback itself.
                time.sleep(0.1)
                start = time.monotonic()
                self.library.szu_campus_cancellation_cancel(token)
                self.assertEqual(future.result(timeout=2), 1, "Cancellation remains distinguishable from success or timeout")
                elapsed = time.monotonic() - start
                self.results.append({"stage": "body" if body_started else "before_headers", "cancel_seconds": elapsed})
                if not self.baseline:
                    self.assertLess(elapsed, 0.3, "Cancellation must wake a slow wait rather than wait for periodic progress")
        finally:
            release.set()
            server.shutdown()
            server.server_close()
            thread.join(timeout=2)
            self.library.szu_campus_cancellation_cancel(token)
            self.library.szu_campus_cancellation_release(token)

    def test_cancel_before_response_headers(self):
        self.measure(False)

    def test_cancel_during_response_body(self):
        self.measure(True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--baseline", action="store_true", help="Record the old transport without the new responsiveness assertion")
    parser.add_argument("--json", type=Path)
    args = parser.parse_args()
    PhysicalCancellationTests.baseline = args.baseline
    result = unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(PhysicalCancellationTests))
    if args.json:
        args.json.write_text(json.dumps(PhysicalCancellationTests.results, indent=2) + "\n")
    raise SystemExit(0 if result.wasSuccessful() else 1)
