"""Exercise configured test discovery through a running language server."""

import json
import os
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
        initialized = self.request("initialize", {"initializationOptions": {
            "sun_configs": [str(self.config)], "entrypoints": [],
        }})
        self.token_types = initialized["capabilities"]["semanticTokensProvider"]["legend"]["tokenTypes"]
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
            "path": entry.name, "name": "suite",
        }]}))

    def test_open_manifest_becomes_entrypoint_until_closed(self):
        """An open manifest supplies context for itself and its source files."""
        self.open_document(self.source)
        self.assertEqual(self.workspace_tests(), [])
        self.open_document(self.entry)
        self.assertEqual(self.document_tests(self.source)["entrypoint"], str(self.entry))
        self.assertEqual(self.document_tests(self.entry)["entrypoint"], str(self.entry))
        suites = self.workspace_tests()
        self.assertEqual(len(suites), 1)
        self.assertEqual(suites[0]["files"][0]["tests"][0]["id"], "suite.first")
        self.configure([self.entry])
        self.assertEqual(len(self.workspace_tests()), 1)
        self.configure([])
        self.assertEqual(len(self.workspace_tests()), 1)
        self.notify("textDocument/didClose", {
            "textDocument": {"uri": self.entry.as_uri()},
        })
        self.assertEqual(self.workspace_tests(), [])

    def diagnostics(self, file):
        """Wait for diagnostics published for the requested document."""
        while True:
            message = self.messages.get(timeout=30)
            if isinstance(message, Exception):
                raise message
            if (message.get("method") == "textDocument/publishDiagnostics"
                    and message["params"]["uri"] == file.as_uri()):
                return message["params"]["diagnostics"]

    def test_unsaved_manifest_resolves_println(self):
        """Adding and removing a manifest updates diagnostics and library hover."""
        source = ('using std;\n/** Prints a greeting. */\n'
                  'function main() i32 { println("hello"); return 0; }\n')
        self.entry.write_text(source)
        self.open_document(self.entry)
        self.assertTrue(any("Unknown variable" in item["message"]
                            for item in self.diagnostics(self.entry)))
        manifest = ('manifest { libraries: ['
                    + json.dumps(str(SERVER.parent / "stdlib.moon")) + '] }\n')
        self.notify("textDocument/didChange", {
            "textDocument": {"uri": self.entry.as_uri(), "version": 2},
            "contentChanges": [{"text": source + manifest}],
        })
        self.assertEqual(self.diagnostics(self.entry), [])
        hover = self.request("textDocument/hover", {
            "textDocument": {"uri": self.entry.as_uri()},
            "position": {"line": 2, "character": source.splitlines()[2].index("println")},
        })
        self.assertIsNotNone(hover)
        self.assertIn("println", hover["contents"]["value"])
        self.assertEqual(self.document_tests(self.entry)["entrypoint"], str(self.entry))
        self.notify("textDocument/didChange", {
            "textDocument": {"uri": self.entry.as_uri(), "version": 3},
            "contentChanges": [{"text": source}],
        })
        self.assertTrue(any("Unknown variable" in item["message"]
                            for item in self.diagnostics(self.entry)))
        self.assertEqual(self.document_tests(self.entry)["entrypoint"], None)

    def test_semantic_tokens_cover_interface_lists(self):
        """Every implemented or extended interface stays a type across commas."""
        for declaration in (
            "class FlyingCar implements IAircraft, ILandcraft",
            "interface IFlyingCar extends IAircraft, ILandcraft",
            "class FlyingCar implements IAircraft, /* ground */\n ILandcraft",
            "class FlyingCar implements IAircraft<Cargo<Box>>, ILandcraft",
            "class FlyingCar implements air.IAircraft, land.ILandcraft",
        ):
            with self.subTest(declaration=declaration):
                source = declaration + " { var fuel = 0; }"
                self.entry.write_text(source)
                self.open_document(self.entry)
                result = self.request("textDocument/semanticTokens/full", {
                    "textDocument": {"uri": self.entry.as_uri()},
                })
                lines = source.splitlines()
                line = column = 0
                classified = {}
                data = result["data"]
                for index in range(0, len(data), 5):
                    delta_line, delta_column, length, kind, _ = data[index:index + 5]
                    line += delta_line
                    column = delta_column if delta_line else column + delta_column
                    classified[lines[line][column:column + length]] = self.token_types[kind]
                self.assertEqual(classified["IAircraft"], "type")
                self.assertEqual(classified["ILandcraft"], "type")
                self.assertEqual(classified["fuel"], "variable")

    def test_config_created_edited_deleted_without_restart(self):
        """Every discovery request reads the latest contents of configured paths."""
        self.assertEqual(self.workspace_tests(), [])
        self.open_document(self.source)
        self.assertEqual(self.document_tests(self.source)["tests"], [])
        self.write_config(self.entry)
        self.assertEqual(self.document_tests(self.source)["entrypoint"], str(self.entry))
        suites = self.workspace_tests()
        self.assertEqual([suite["entrypoint"] for suite in suites], [str(self.entry)])
        self.assertTrue(Path(suites[0]["test_binary"]).is_relative_to(self.directory / ".sun" / "build"))
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

    def test_target_name_determines_test_binary(self):
        """Discovery resolves configured paths so the editor can use built tests."""
        self.config.write_text(json.dumps({
            "path_variables": {"TEST_SOURCE": ".", "TEST_BUILD": "build"},
            "entrypoints": [{
                "path": "$TEST_SOURCE/main.sun",
                "name": "suite",
            }],
        }))
        suites = self.workspace_tests()
        self.assertEqual(len(suites), 1)
        self.assertEqual(Path(suites[0]["entrypoint"]).resolve(), self.entry)
        self.assertTrue(Path(suites[0]["test_binary"]).is_relative_to(self.directory / ".sun" / "build"))
        self.assertEqual(Path(suites[0]["test_binary"]).name, "suite_test")
        self.assertEqual(suites[0]["files"][0]["tests"][0]["id"], "suite.first")

    def test_composed_config_discovers_included_suites(self):
        """An ordinary config includes child suites with child-relative paths."""
        child = self.directory / "child"
        child.mkdir()
        (child / "sun-config.json").write_text(json.dumps({
            "package_id": "example/tests",
            "entrypoints": [{"path": "../main.sun",
                             "name": "suite"}],
        }))
        self.config.write_text(json.dumps({"projects": ["child/sun-config.json"]}))
        suites = self.workspace_tests()
        self.assertEqual(len(suites), 1)
        self.assertEqual(Path(suites[0]["entrypoint"]).resolve(), self.entry)
        self.assertTrue(Path(suites[0]["test_binary"]).is_relative_to(
            self.directory / ".sun" / "build"))
        self.assertEqual(suites[0]["files"][0]["tests"][0]["id"], "suite.first")

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

    def test_language_features_in_merged_modules(self):
        """Every file in a shared module supports symbol lookup and edits."""
        first = self.directory / "first.sun"
        second = self.directory / "second.sun"
        first.write_text(
            "/** Shares declarations. */ module shared { "
            "/** Returns one. */ function first() i32 { return 1; } }\n"
        )
        second.write_text(
            "// Exercise a symbol in a later file of the shared module.\n"
            "/** Shares declarations. */ module shared {\n"
            "    /** Returns two. */ function value() i32 { return 2; }\n"
            "    /** Calls the shared function. */ function use() i32 { return value(); }\n"
            "}\n"
        )
        self.entry.write_text(
            'manifest { source_files: ["first.sun", "second.sun"] }\n'
        )
        self.configure([self.entry])
        for file, line, symbol in ((first, 0, "first"), (second, 2, "value")):
            self.open_document(file)
            character = file.read_text().splitlines()[line].index(symbol)
            params = {
                "textDocument": {"uri": file.as_uri()},
                "position": {"line": line, "character": character},
            }
            with self.subTest(file=file.name):
                hover = self.request("textDocument/hover", params)
                self.assertIsNotNone(hover)
                self.assertIn(symbol, hover["contents"]["value"])
                definition = self.request("textDocument/definition", params)
                self.assertIsNotNone(definition)
                references = self.request("textDocument/references", {
                    **params, "context": {"includeDeclaration": True},
                })
                expected = 1 if file == first else 2
                self.assertEqual(len(references), expected)
                prepared = self.request("textDocument/prepareRename", params)
                self.assertEqual(prepared["placeholder"], symbol)
                renamed = self.request("textDocument/rename", {
                    **params, "newName": "renamed",
                })
                edits = renamed["changes"][file.as_uri()]
                self.assertEqual(len(edits), expected)
                self.assertTrue(all(edit["newText"] == "renamed" for edit in edits))

    def test_inventory_reloads_composed_configs_without_source_analysis(self):
        """Created, changed, or removed child configs update the lightweight inventory."""
        inventory = self.request("sun/testSuites")
        self.assertEqual(inventory["suites"], [])
        child = self.directory / "child.json"
        self.config.write_text(json.dumps({"projects": [child.name]}))
        child.write_text(json.dumps({"entrypoints": [{"path": self.entry.name, "name": "first"}]}))
        self.source.write_text("test_function syntaxError( {\n")
        inventory = self.request("sun/testSuites")
        self.assertEqual(inventory["suites"][0]["entrypoint"], str(self.entry))
        self.assertIn(str(child), inventory["watched_paths"])
        self.assertIn(str(self.source), inventory["watched_paths"])
        child.write_text(json.dumps({"entrypoints": [{"path": self.other.name, "name": "second"}]}))
        self.assertEqual(self.request("sun/testSuites")["suites"][0]["entrypoint"], str(self.other))
        child.unlink()
        inventory = self.request("sun/testSuites")
        self.assertEqual(inventory["suites"], [])
        self.assertTrue(inventory["errors"])
        self.assertIn(str(child), inventory["watched_paths"])
        self.config.write_text(json.dumps({"projects": ["children/*.json"]}))
        self.assertIn(str(self.directory / "children/*.json"),
                      self.request("sun/testSuites")["watched_patterns"])

    def test_binary_inventory_and_listing_freshness(self):
        """Inventory reads build capability and flags edits without analyzing test bodies."""
        self.write_config(self.entry)
        inventory = self.request("sun/testSuites")
        suite = inventory["suites"][0]
        self.assertEqual(suite["freshness"], "missing")
        self.assertEqual(suite["listing_version"], 0)
        self.assertIn(str(self.config), inventory["watched_paths"])
        binary = Path(suite["test_binary"])
        binary.parent.mkdir(parents=True, exist_ok=True)
        # A quote in the source path exercises both generated Sun and JSON escaping.
        quoted = self.directory / 'quoted"tests.sun'
        quoted.write_text("module outer { module inner { /* π😀 */ test_function fails() { "
                          "try std.test.assert(false); } } }\n")
        self.entry.write_text('manifest { test_files: [' + json.dumps(quoted.name)
                             + '] libraries: [' + json.dumps(str(SERVER.parent / "stdlib.moon"))
                             + '] }\n')
        result = subprocess.run(
            [str(COMPILER), "-c", "-O0", "-o", str(binary)[:-5], str(self.entry)],
            cwd=WORKSPACE, capture_output=True, text=True, timeout=90,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        suite = self.request("sun/testSuites")["suites"][0]
        self.assertEqual(suite["freshness"], "fresh", suite)
        self.assertEqual(suite["listing_version"], 1)
        self.assertIsNotNone(suite["binary_identity"])
        outputs = [subprocess.run([str(binary), "--list-tests"], capture_output=True,
                                  text=True, timeout=10) for _ in range(2)]
        for output in outputs:
            self.assertEqual(output.returncode, 0, output.stderr)
        self.assertEqual(outputs[0].stdout, outputs[1].stdout)
        listing = json.loads(outputs[0].stdout)
        self.assertEqual(listing["version"], 1)
        case = listing["tests"][0]
        self.assertEqual(case["id"], "outer.inner.fails")
        self.assertEqual(case["file"], str(quoted))
        self.assertEqual(case["range"]["start"]["line"], 0)
        prefix = quoted.read_text().split("fails", 1)[0]
        self.assertEqual(case["range"]["start"]["character"], len(prefix.encode("utf-16-le")) // 2)
        executed = subprocess.run([str(binary)], capture_output=True, text=True, timeout=10)
        self.assertEqual(executed.returncode, 1, executed.stdout)
        self.assertIn("FAIL outer.inner.fails", executed.stdout)
        # A semantic error does not prevent inventory or execution of the old binary.
        quoted.write_text("test_function changed() { nonexistent(); }\n")
        newer = binary.stat().st_mtime_ns + 1000000000
        os.utime(quoted, ns=(newer, newer))
        suite = self.request("sun/testSuites")["suites"][0]
        self.assertEqual(suite["freshness"], "stale", suite)
        self.assertEqual(suite["listing_version"], 1)
        self.assertEqual(subprocess.check_output([str(binary), "--list-tests"], text=True),
                         outputs[0].stdout)
        quoted.unlink()
        self.assertEqual(self.request("sun/testSuites")["suites"][0]["freshness"], "unknown")
        binary.unlink()
        self.assertEqual(self.request("sun/testSuites")["suites"][0]["freshness"], "missing")

    def test_inventory_tracks_unsaved_inputs_and_legacy_binaries(self):
        """Unsaved input text is stale and arbitrary executables have no listing capability."""
        self.write_config(self.entry)
        suite = self.request("sun/testSuites")["suites"][0]
        binary = Path(suite["test_binary"])
        binary.parent.mkdir(parents=True, exist_ok=True)
        binary.write_text("not a supported binary")
        self.assertEqual(self.request("sun/testSuites")["suites"][0]["listing_version"], 0)
        self.open_document(self.source)
        self.notify("textDocument/didChange", {"textDocument": {
            "uri": self.source.as_uri(), "version": 2,
        }, "contentChanges": [{"text": "module suite { test_function edited() { return; } }"}]})
        suite = self.request("sun/testSuites")["suites"][0]
        self.assertEqual(suite["freshness"], "stale", suite)
        self.assertIn("Unsaved changes", suite["reason"])
        self.notify("textDocument/didClose", {"textDocument": {"uri": self.source.as_uri()}})
        self.assertEqual(self.request("sun/testSuites")["suites"][0]["freshness"], "fresh")
        newer = binary.stat().st_mtime_ns + 1000000000
        os.utime(self.config, ns=(newer, newer))
        self.assertEqual(self.request("sun/testSuites")["suites"][0]["freshness"], "stale")

    def test_invalid_config_cannot_claim_freshness_for_other_suites(self):
        """A readable but invalid participating config leaves existing binaries unknown."""
        self.write_config(self.entry)
        self.entry.write_text('manifest { test_files: ["tests.sun"] }\n')
        invalid = self.directory / "invalid.json"
        invalid.write_text("{invalid")
        self.configure([], [self.config, invalid])
        suite = self.request("sun/testSuites")["suites"][0]
        binary = Path(suite["test_binary"])
        binary.parent.mkdir(parents=True, exist_ok=True)
        binary.write_text("legacy binary")
        inventory = self.request("sun/testSuites")
        self.assertTrue(inventory["errors"])
        self.assertEqual(inventory["suites"][0]["freshness"], "unknown")
        newer = binary.stat().st_mtime_ns + 1000000000
        os.utime(self.entry, ns=(newer, newer))
        self.assertEqual(self.request("sun/testSuites")["suites"][0]["freshness"], "stale")

    def test_inventory_hides_fresh_targets_known_to_have_no_tests(self):
        """Build records suppress non-test targets until an input changes."""
        self.write_config(self.entry)
        self.source.write_text("function helper() i32 { return 0; }\n")
        self.entry.write_text("function main() i32 { return 0; }\n"
                             + self.entry.read_text().replace("test_files:", "source_files:"))
        binary = Path(self.request("sun/testSuites")["suites"][0]["test_binary"])
        output = Path(str(binary)[:-5])
        output.parent.mkdir(parents=True, exist_ok=True)
        result = subprocess.run([str(COMPILER), "-c", "-O0", "-o", str(output),
                                 str(self.entry)], cwd=WORKSPACE, capture_output=True,
                                text=True, timeout=90)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        inventory = self.request("sun/testSuites")
        self.assertEqual(inventory["suites"], [])
        self.assertIn(str(binary), inventory["watched_binaries"])
        self.assertIn(str(output), inventory["watched_binaries"])
        newer = output.stat().st_mtime_ns + 1000000000
        os.utime(self.source, ns=(newer, newer))
        self.assertEqual(self.request("sun/testSuites")["suites"][0]["freshness"], "missing")

    def composed_candidates(self):
        """Create one package reachable through workspace and standalone roots."""
        project = self.directory / "project"
        project.mkdir()
        entry = project / "main.sun"
        entry.write_text(self.entry.read_text().replace('"tests.sun"', json.dumps(str(self.source))))
        config = project / "sun-config.json"
        config.write_text(json.dumps({"root": True, "package_id": "example/library",
                                      "entrypoints": [{"path": "main.sun", "name": "unit"}]}))
        self.config.write_text(json.dumps({"projects": [str(config)]}))
        inventory = self.request("sun/testSuites")
        suite = inventory["suites"][0]
        workspace = Path(suite["test_binary"])
        standalone = project / workspace.relative_to(self.directory)
        return entry, workspace, standalone

    def test_inventory_selects_build_locations_and_watches_both(self):
        """Fresh standalone builds beat stale workspace builds; fresh ties prefer workspace."""
        entry, workspace, standalone = self.composed_candidates()
        for binary in (workspace, standalone):
            binary.parent.mkdir(parents=True, exist_ok=True)
        standalone.write_text("standalone fixture")
        suite = self.request("sun/testSuites")["suites"][0]
        self.assertEqual(suite["test_binary"], str(standalone))
        self.assertEqual(suite["package_id"], "example/library")
        self.assertEqual(suite["entrypoint_name"], "unit")
        self.assertEqual(suite["build_location"], "standalone")
        workspace.write_text("workspace fixture")
        old = entry.stat().st_mtime_ns - 1000000000
        os.utime(workspace, ns=(old, old))
        self.assertEqual(self.request("sun/testSuites")["suites"][0]["test_binary"], str(standalone))
        newer = standalone.stat().st_mtime_ns + 1000000000
        os.utime(workspace, ns=(newer, newer))
        inventory = self.request("sun/testSuites")
        self.assertEqual(inventory["suites"][0]["test_binary"], str(workspace))
        self.assertIn(str(workspace), inventory["watched_binaries"])
        self.assertIn(str(standalone), inventory["watched_binaries"])
        workspace.unlink()
        self.assertEqual(self.request("sun/testSuites")["suites"][0]["test_binary"], str(standalone))

    def test_workspace_config_changes_do_not_stale_standalone_builds(self):
        """Only the selected build's configuration contributes to its freshness."""
        entry, workspace, standalone = self.composed_candidates()
        for binary in (workspace, standalone):
            binary.parent.mkdir(parents=True, exist_ok=True)
            binary.write_text("test fixture")
        newer = standalone.stat().st_mtime_ns + 1000000000
        os.utime(self.config, ns=(newer, newer))
        suite = self.request("sun/testSuites")["suites"][0]
        self.assertEqual(suite["test_binary"], str(standalone))
        self.assertEqual(suite["freshness"], "fresh")
        self.assertNotIn(str(self.config), suite["sources"])
        self.assertIn(str(self.config), self.request("sun/testSuites")["watched_paths"])

    def test_inventory_resolves_imports_in_each_build_directory(self):
        """A stale workspace dependency must not mark a standalone build stale."""
        entry, workspace, standalone = self.composed_candidates()
        entry.write_text('manifest { libraries: ["${SUN_BUILD}/fixture.moon"] }\n')
        for binary in (workspace, standalone):
            binary.parent.mkdir(parents=True, exist_ok=True)
            (binary.parent / "fixture.moon").write_text("dependency fixture")
            binary.write_text("test fixture")
        dependency = workspace.parent / "fixture.moon"
        newer = workspace.stat().st_mtime_ns + 1000000000
        os.utime(dependency, ns=(newer, newer))
        inventory = self.request("sun/testSuites")
        suite = inventory["suites"][0]
        self.assertEqual(suite["test_binary"], str(standalone))
        self.assertEqual(suite["freshness"], "fresh")
        self.assertIn(str(standalone.parent / "fixture.moon"), suite["sources"])
        self.assertIn(str(dependency), inventory["watched_paths"])

    def test_standalone_non_test_metadata_hides_unbuilt_workspace_target(self):
        """A standalone production build can prove a benchmark has no tests."""
        entry, workspace, standalone = self.composed_candidates()
        entry.write_text("manifest {}\nfunction main() i32 { return 0; }\n")
        output = Path(str(standalone)[:-5])
        output.parent.mkdir(parents=True, exist_ok=True)
        result = subprocess.run([str(COMPILER), "-c", "-O0", "-o", str(output), str(entry)],
                                cwd=WORKSPACE, capture_output=True, text=True, timeout=90)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        inventory = self.request("sun/testSuites")
        self.assertEqual(inventory["suites"], [])
        self.assertIn(str(workspace), inventory["watched_binaries"])
        self.assertIn(str(standalone), inventory["watched_binaries"])

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
