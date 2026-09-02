// testExplorer.ts — Test Explorer integration for Sun's test_function tests.
//
// Discovery is workspace-wide: the language server's custom
// "sun/workspaceTests" request lists every test of every configured
// entrypoint (from the sun_configs setting's entrypoints lists), so the tree
// populates without opening a single file. The per-document "sun/tests"
// request keeps one file's subtree current as it is edited. The tree groups
// tests by entrypoint, then by file.
//
// Running prefers the entrypoint's configured test binary
// (test_binary_name in sun-config.json) when it is newer than every source;
// otherwise it JIT-compiles via `sun test <entrypoint>`. Either way,
// selections travel as --test-filter arguments, and the runner's PASS/FAIL
// lines map back onto the test items.

import * as fs from 'node:fs';
import * as path from 'node:path';
import { spawn } from 'node:child_process';
import * as vscode from 'vscode';
import { LanguageClient } from 'vscode-languageclient/node';

interface TestRange {
  start: { line: number; character: number };
  end: { line: number; character: number };
}

interface SunTestsResponse {
  entrypoint: string;
  tests: Array<{ id: string; label: string; range: TestRange }>;
}

interface WorkspaceTestsResponse {
  entrypoints: Array<{
    entrypoint: string;
    test_binary: string | null;
    sources: string[];
    files: Array<{
      uri: string;
      tests: Array<{ id: string; label: string; range: TestRange }>;
    }>;
  }>;
}

/** Where a leaf test item points: its runner name and how to run it. */
interface TestTarget {
  entrypoint: string;
  dottedName: string;
}

/** How to run one entrypoint's tests: the prebuilt binary (when declared
 *  in a sun-config) and the sources that decide whether it is fresh. */
interface EntrypointRun {
  testBinary: string | null;
  sources: string[];
}

/** Resolve the `sun` compiler like the server command: absolute path,
 *  workspace-relative path, then PATH. Falls back to a `sun` binary sitting
 *  next to the resolved sun-lsp, which is where the build puts both. */
function resolveSunBinary(serverCommand: string): string {
  const configured = vscode.workspace
    .getConfiguration('sun')
    .get<string>('compiler_path', 'sun');

  if (path.isAbsolute(configured) && fs.existsSync(configured)) {
    return configured;
  }
  const workspaceFolder = vscode.workspace.workspaceFolders?.[0]?.uri.fsPath;
  if (workspaceFolder) {
    const local = path.join(workspaceFolder, configured);
    if (fs.existsSync(local)) {
      return local;
    }
  }
  if (path.isAbsolute(serverCommand)) {
    const sibling = path.join(path.dirname(serverCommand), 'sun');
    if (fs.existsSync(sibling)) {
      return sibling;
    }
  }
  return configured;
}

/** True when the prebuilt test binary exists and is at least as new as
 *  every source of its entrypoint, so running it tests the current code. */
function binaryIsFresh(run: EntrypointRun): boolean {
  if (!run.testBinary) return false;
  try {
    const binaryTime = fs.statSync(run.testBinary).mtimeMs;
    for (const source of run.sources) {
      if (fs.statSync(source).mtimeMs > binaryTime) {
        return false;
      }
    }
    return true;
  } catch {
    return false;
  }
}

export function activateTestExplorer(
  context: vscode.ExtensionContext,
  client: LanguageClient,
  env: NodeJS.ProcessEnv,
  workspaceFolder: string | undefined,
  serverCommand: string
): void {
  const controller = vscode.tests.createTestController('sun', 'Sun Tests');
  const output = vscode.window.createOutputChannel('Sun Tests');
  const targets = new WeakMap<vscode.TestItem, TestTarget>();
  const entrypointRuns = new Map<string, EntrypointRun>();

  context.subscriptions.push(controller);
  context.subscriptions.push(output);

  const entrypointLabel = (entrypoint: string): string =>
    workspaceFolder ? path.relative(workspaceFolder, entrypoint) : entrypoint;

  function makeTestItem(
    entrypoint: string,
    uri: vscode.Uri,
    test: { id: string; label: string; range: TestRange }
  ): vscode.TestItem {
    const item = controller.createTestItem(
      `${entrypoint}::${test.id}`,
      test.label,
      uri
    );
    item.range = new vscode.Range(
      test.range.start.line,
      test.range.start.character,
      test.range.end.line,
      test.range.end.character
    );
    targets.set(item, { entrypoint, dottedName: test.id });
    return item;
  }

  function makeFileItem(
    entrypoint: string,
    uri: vscode.Uri,
    tests: Array<{ id: string; label: string; range: TestRange }>
  ): vscode.TestItem {
    const fileItem = controller.createTestItem(
      uri.fsPath,
      path.basename(uri.fsPath),
      uri
    );
    targets.set(fileItem, { entrypoint, dottedName: '' });
    for (const test of tests) {
      fileItem.children.add(makeTestItem(entrypoint, uri, test));
    }
    return fileItem;
  }

  /** Rebuild the whole tree from the server's workspace-wide listing. */
  async function refreshWorkspace(): Promise<void> {
    let response: WorkspaceTestsResponse;
    try {
      response = await client.sendRequest<WorkspaceTestsResponse>(
        'sun/workspaceTests',
        {}
      );
    } catch {
      return;
    }

    entrypointRuns.clear();
    const roots: vscode.TestItem[] = [];
    for (const entry of response.entrypoints) {
      entrypointRuns.set(entry.entrypoint, {
        testBinary: entry.test_binary,
        sources: entry.sources,
      });
      if (entry.files.length === 0) continue;
      const root = controller.createTestItem(
        entry.entrypoint,
        entrypointLabel(entry.entrypoint),
        vscode.Uri.file(entry.entrypoint)
      );
      targets.set(root, { entrypoint: entry.entrypoint, dottedName: '' });
      for (const file of entry.files) {
        const uri = vscode.Uri.parse(file.uri);
        root.children.add(makeFileItem(entry.entrypoint, uri, file.tests));
      }
      roots.push(root);
    }
    controller.items.replace(roots);
  }

  /** Ask the server for the tests in one open document and rebuild that
   *  file's subtree, so edits show up without a full workspace pass. */
  async function refreshDocument(document: vscode.TextDocument): Promise<void> {
    if (document.languageId !== 'sun' || document.uri.scheme !== 'file') {
      return;
    }
    let response: SunTestsResponse;
    try {
      response = await client.sendRequest<SunTestsResponse>('sun/tests', {
        textDocument: { uri: document.uri.toString() },
      });
    } catch {
      return;
    }

    const filePath = document.uri.fsPath;
    const entrypoint = response.entrypoint || filePath;

    // Drop the file's previous subtree wherever it was (its entrypoint may
    // have changed when a manifest appeared or vanished).
    for (const [, root] of controller.items) {
      root.children.delete(filePath);
      if (root.children.size === 0) {
        controller.items.delete(root.id);
      }
    }
    if (response.tests.length === 0) {
      return;
    }

    let root = controller.items.get(entrypoint);
    if (!root) {
      root = controller.createTestItem(
        entrypoint,
        entrypointLabel(entrypoint),
        vscode.Uri.file(entrypoint)
      );
      targets.set(root, { entrypoint, dottedName: '' });
      controller.items.add(root);
    }
    root.children.add(makeFileItem(entrypoint, document.uri, response.tests));
  }

  controller.refreshHandler = refreshWorkspace;

  // Entrypoints and tests change when sun-config.json does.
  const configWatcher =
    vscode.workspace.createFileSystemWatcher('**/sun-config.json');
  configWatcher.onDidCreate(() => refreshWorkspace());
  configWatcher.onDidChange(() => refreshWorkspace());
  configWatcher.onDidDelete(() => refreshWorkspace());
  context.subscriptions.push(configWatcher);

  context.subscriptions.push(
    vscode.workspace.onDidOpenTextDocument(refreshDocument),
    vscode.workspace.onDidSaveTextDocument(refreshDocument)
  );

  /** Leaf test items under an item (a leaf returns itself). */
  function leavesOf(item: vscode.TestItem): vscode.TestItem[] {
    if (item.children.size === 0) {
      return [item];
    }
    const leaves: vscode.TestItem[] = [];
    item.children.forEach((child) => leaves.push(...leavesOf(child)));
    return leaves;
  }

  async function runTests(
    request: vscode.TestRunRequest,
    token: vscode.CancellationToken
  ): Promise<void> {
    const run = controller.createTestRun(request);
    const sunBinary = resolveSunBinary(serverCommand);

    // Collect the requested leaves, grouped by entrypoint.
    const requested: vscode.TestItem[] = [];
    if (request.include) {
      for (const item of request.include) {
        requested.push(...leavesOf(item));
      }
    } else {
      controller.items.forEach((root) => requested.push(...leavesOf(root)));
    }
    const excluded = new Set(request.exclude ?? []);
    const byEntrypoint = new Map<string, vscode.TestItem[]>();
    for (const item of requested) {
      if (excluded.has(item)) continue;
      const target = targets.get(item);
      if (!target || !target.dottedName) continue;
      const group = byEntrypoint.get(target.entrypoint) ?? [];
      group.push(item);
      byEntrypoint.set(target.entrypoint, group);
    }

    for (const [entrypoint, items] of byEntrypoint) {
      if (token.isCancellationRequested) break;

      // No filter args when every known test of the entrypoint is included:
      // the suite may hold tests in files not yet discovered, and run-all
      // should run those too.
      const root = controller.items.get(entrypoint);
      const knownLeaves = root ? leavesOf(root) : [];
      const runsWholeSuite =
        knownLeaves.length > 0 && knownLeaves.every((leaf) => items.includes(leaf));

      const filterArgs: string[] = [];
      if (!runsWholeSuite) {
        for (const item of items) {
          filterArgs.push('--test-filter', targets.get(item)!.dottedName);
        }
      }

      // A fresh prebuilt test binary (declared in sun-config.json) runs
      // instantly; anything else JIT-compiles the current sources.
      const runInfo = entrypointRuns.get(entrypoint);
      let command: string;
      let args: string[];
      if (runInfo && binaryIsFresh(runInfo)) {
        command = runInfo.testBinary!;
        args = filterArgs;
      } else {
        command = sunBinary;
        args = ['test', entrypoint, ...filterArgs];
      }

      for (const item of items) run.started(item);
      const byName = new Map<string, vscode.TestItem>();
      for (const item of items) byName.set(targets.get(item)!.dottedName, item);

      output.appendLine(`> ${command} ${args.join(' ')}`);
      await new Promise<void>((resolve) => {
        const child = spawn(command, args, {
          cwd: workspaceFolder ?? path.dirname(entrypoint),
          env,
        });
        token.onCancellationRequested(() => child.kill());

        let buffered = '';
        let allOutput = '';
        const handleLine = (line: string) => {
          run.appendOutput(line + '\r\n');
          const pass = /^PASS (\S+)$/.exec(line);
          if (pass) {
            const item = byName.get(pass[1]);
            if (item) {
              run.passed(item);
              byName.delete(pass[1]);
            }
            return;
          }
          const fail = /^FAIL (\S+): ?(.*)$/.exec(line);
          if (fail) {
            const item = byName.get(fail[1]);
            if (item) {
              run.failed(item, new vscode.TestMessage(fail[2] || 'test failed'));
              byName.delete(fail[1]);
            }
          }
        };
        const consume = (chunk: Buffer) => {
          allOutput += chunk.toString();
          buffered += chunk.toString();
          let newline;
          while ((newline = buffered.indexOf('\n')) >= 0) {
            handleLine(buffered.slice(0, newline).replace(/\r$/, ''));
            buffered = buffered.slice(newline + 1);
          }
        };
        child.stdout.on('data', consume);
        child.stderr.on('data', (chunk: Buffer) => {
          allOutput += chunk.toString();
          run.appendOutput(chunk.toString().replace(/\n/g, '\r\n'));
        });
        child.on('error', (error) => {
          for (const item of byName.values()) {
            run.errored(item, new vscode.TestMessage(String(error)));
          }
          byName.clear();
          resolve();
        });
        child.on('close', () => {
          if (buffered.length > 0) handleLine(buffered.replace(/\r$/, ''));
          // A requested test the runner never reported: a compile failure,
          // a crash, or a name the filter did not match.
          for (const item of byName.values()) {
            run.errored(
              item,
              new vscode.TestMessage(
                allOutput.trim() || 'the test runner reported no result'
              )
            );
          }
          resolve();
        });
      });
    }

    run.end();
  }

  controller.createRunProfile(
    'Run',
    vscode.TestRunProfileKind.Run,
    runTests,
    true
  );

  void refreshWorkspace();
}
