"""Exercise configured test discovery through a running language server."""

import json
from pathlib import Path
import queue
import subprocess
import sys
import tempfile
import threading
import unittest


SERVER = Path(sys.argv.pop(1)).resolve()
COMPILER = SERVER.with_name("sun")
WORKSPACE = Path(__file__).resolve().parents[3]


class DiscoveryProtocolTests(unittest.TestCase):
    """Check discovery as configuration and source files change on disk."""

    def setUp(self):
        """Create an isolated project and start the server from the workspace."""
        (WORKSPACE / "tmp").mkdir(exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(dir=WORKSPACE / "tmp")
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name)
        self.config = self.directory / "custom.json"
        (self.directory / "sun-config.json").write_text('{"root": true}')
        self.source = self.directory / "tests.sun"
        self.source.write_text("module suite { test_function first() { return; } }\n")
        self.entry = self.directory / "main.sun"
        self.other = self.directory / "other.sun"
        for entry in (self.entry, self.other):
            entry.write_text(
                'manifest { test_files: ["tests.sun"] libraries: ['
                + json.dumps(str(SERVER.parent / "stdlib.moon")) + '] }\n'
            )
        self.errors = tempfile.TemporaryFile(dir=WORKSPACE / "tmp")
        self.addCleanup(self.errors.close)
        self.process = subprocess.Popen(
            [str(SERVER)], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=self.errors, cwd=WORKSPACE,
        )
        self.addCleanup(self.stop_server)
        self.messages = queue.Queue()
        self.reader = threading.Thread(target=self.read_messages, daemon=True)
        self.reader.start()
        self.request_id = 0
        self.request("initialize", {"initializationOptions": {
            "sun_configs": [str(self.config)], "entrypoints": [],
        }})
        self.notify("initialized", {})

    def stop_server(self):
        """Reap the server and close its pipes even after a failed assertion."""
        self.process.terminate()
        self.process.wait(timeout=10)
        self.reader.join(timeout=10)
        self.process.stdin.close()
        self.process.stdout.close()

    def read_messages(self):
        """Read framed responses while letting requests enforce a timeout."""
        try:
            while True:
                headers = {}
                while True:
                    line = self.process.stdout.readline()
                    if not line:
                        raise EOFError("language server closed stdout")
                    if line == b"\r\n":
                        break
                    name, value = line.decode().split(":", 1)
                    headers[name.lower()] = value.strip()
                size = int(headers["content-length"])
                self.messages.put(json.loads(self.process.stdout.read(size)))
        except Exception as error:
            self.messages.put(error)

    def send(self, message):
        """Write a framed JSON message to the server."""
        body = json.dumps({"jsonrpc": "2.0", **message}).encode()
        self.process.stdin.write(f"Content-Length: {len(body)}\r\n\r\n".encode() + body)
        self.process.stdin.flush()

    def notify(self, method, params):
        """Send a notification without waiting for a response."""
        self.send({"method": method, "params": params})

    def request(self, method, params=None):
        """Wait for the matching response, ignoring diagnostic notifications."""
        self.request_id += 1
        self.send({"id": self.request_id, "method": method, "params": params or {}})
        while True:
            message = self.messages.get(timeout=30)
            if isinstance(message, Exception):
                raise message
            if message.get("id") == self.request_id:
                self.assertNotIn("error", message)
                return message["result"]

    def open_document(self, file):
        """Open a source file without treating it as a configured entrypoint."""
        self.notify("textDocument/didOpen", {"textDocument": {
            "uri": file.as_uri(), "languageId": "sun", "version": 1,
            "text": file.read_text(),
        }})

    def configure(self, entries, configs=None):
        """Replace the explicit entrypoints and configured config paths."""
        self.notify("workspace/didChangeConfiguration", {"settings": {"sun": {
            "entrypoints": [str(entry) for entry in entries],
            "sun_configs": [str(config) for config in (configs or [])],
        }}})

    def workspace_tests(self):
        """Return the entrypoint groups discovered by the server."""
        return self.request("sun/workspaceTests")["entrypoints"]

    def document_tests(self, file):
        """Request tests for an already open document."""
        return self.request("sun/tests", {"textDocument": {"uri": file.as_uri()}})

    def write_config(self, entry):
        """Declare a suite and its optional prebuilt test binary."""
        self.config.write_text(json.dumps({"entrypoints": [{
            "path": entry.name, "test_binary_name": "suite_test",
        }]}))

    def test_unconfigured_open_files_never_become_suites(self):
        """Neither a test file nor a manifest supplies an implicit entrypoint."""
        for file in (self.source, self.entry):
            self.open_document(file)
            self.assertEqual(self.workspace_tests(), [])
            self.assertEqual(self.document_tests(file), {"entrypoint": None, "tests": []})

    def test_config_created_edited_deleted_without_restart(self):
        """Every discovery request reads the latest contents of configured paths."""
        self.assertEqual(self.workspace_tests(), [])
        self.open_document(self.source)
        self.assertEqual(self.document_tests(self.source)["tests"], [])
        self.write_config(self.entry)
        self.assertEqual(self.document_tests(self.source)["entrypoint"], str(self.entry))
        suites = self.workspace_tests()
        self.assertEqual([suite["entrypoint"] for suite in suites], [str(self.entry)])
        self.assertEqual(suites[0]["test_binary"], str(self.directory / "suite_test"))
        self.assertEqual(suites[0]["files"][0]["tests"][0]["id"], "suite.first")
        self.write_config(self.other)
        self.assertEqual(self.workspace_tests()[0]["entrypoint"], str(self.other))
        self.assertEqual(self.document_tests(self.source)["entrypoint"], str(self.other))
        self.config.unlink()
        self.assertEqual(self.workspace_tests(), [])
        self.assertEqual(self.document_tests(self.source), {"entrypoint": None, "tests": []})
        self.config.write_text("invalid JSON")
        self.assertEqual(self.workspace_tests(), [])
        self.write_config(self.entry)
        self.assertEqual(len(self.workspace_tests()), 1)

    def test_explicit_and_configured_suites_share_updated_sources(self):
        """Both settings supply suites, including tests in unopened files."""
        self.configure([self.entry])
        self.assertEqual(self.workspace_tests()[0]["entrypoint"], str(self.entry))
        self.write_config(self.other)
        self.configure([self.entry], [self.config])
        self.assertEqual(len(self.workspace_tests()), 2)
        self.source.write_text("module suite { test_function changed() { return; } }\n")
        for suite in self.workspace_tests():
            self.assertEqual(suite["files"][0]["tests"][0]["id"], "suite.changed")
        self.open_document(self.source)
        self.notify("textDocument/didChange", {
            "textDocument": {"uri": self.source.as_uri(), "version": 2},
            "contentChanges": [{"text": "module suite { test_function unsaved() { return; } }\n"}],
        })
        for suite in self.workspace_tests():
            self.assertEqual(suite["files"][0]["tests"][0]["id"], "suite.unsaved")
        self.entry.write_text("manifest {}\n")
        suites = {suite["entrypoint"]: suite for suite in self.workspace_tests()}
        self.assertEqual(suites[str(self.entry)]["files"], [])
        self.assertEqual(len(suites[str(self.other)]["files"]), 1)
        self.configure([])
        self.assertEqual(self.workspace_tests(), [])

    def test_test_file_runs_through_its_configured_entrypoint(self):
        """The discovered entrypoint and dotted name form a working test command."""
        self.configure([self.entry])
        suite = self.workspace_tests()[0]
        dotted_name = suite["files"][0]["tests"][0]["id"]
        result = subprocess.run(
            [str(COMPILER), "test", suite["entrypoint"], "--test-filter", dotted_name],
            cwd=WORKSPACE, capture_output=True, text=True, timeout=60,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("PASS suite.first", result.stdout)


if __name__ == "__main__":
    unittest.main()
