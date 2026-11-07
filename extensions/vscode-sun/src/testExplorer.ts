/** Discovers compiled tests and runs binaries with explicit freshness warnings. */
import * as fs from 'node:fs';
import * as path from 'node:path';
import * as os from 'node:os';
import { spawn } from 'node:child_process';
import { StringDecoder } from 'node:string_decoder';
import * as vscode from 'vscode';
import { LanguageClient } from 'vscode-languageclient/node';

/** Source range recorded by the compiler in UTF-16 coordinates. */
interface TestRange {
  start: { line: number; character: number };
  end: { line: number; character: number };
}
/** One test in the binary's versioned listing. */
interface BinaryTest { id: string; label: string; file: string; range: TestRange }
/** Expected suite and current freshness supplied by the LSP. */
interface Suite {
  entrypoint: string;
  test_binary: string | null;
  listing_version: number;
  binary_identity: string | null;
  freshness: 'fresh' | 'stale' | 'missing' | 'unknown';
  reason: string;
  sources: string[];
}
/** Lightweight project inventory without source test declarations. */
interface SuiteResponse { suites: Suite[]; watched_paths: string[]; watched_binaries?: string[]; watched_patterns?: string[]; errors: string[] }
/** Connects editor items with the runner's test names. */
interface TestTarget { entrypoint: string; dottedName: string }
/** Runtime state for a discovered suite. */
interface EntrypointRun { testBinary: string | null; identity?: string; suite: Suite }

/** Detects binary replacement even when modification time is preserved. */
function binaryIdentity(file: string): string {
  const stat = fs.statSync(file, { bigint: true });
  if (!stat.isFile()) throw new Error(`Not a test binary: ${file}`);
  return [stat.dev, stat.ino, stat.size, stat.mtimeNs, stat.ctimeNs].join(':');
}

/** Validates runner output before adding editor items. */
function parseListing(text: string): BinaryTest[] {
  const listing = JSON.parse(text);
  if (listing.version !== 1 || !Array.isArray(listing.tests)) {
    throw new Error('Unsupported test listing; rebuild this suite');
  }
  const ids = new Set<string>();
  for (const test of listing.tests) {
    if (!test || typeof test.id !== 'string' || !test.id || ids.has(test.id)
        || typeof test.label !== 'string' || typeof test.file !== 'string'
        || (test.file !== '' && !path.isAbsolute(test.file))) {
      throw new Error('Invalid test identity or source path');
    }
    ids.add(test.id);
    for (const point of [test.range?.start, test.range?.end]) {
      if (!point || !Number.isSafeInteger(point.line) || point.line < 0
          || !Number.isSafeInteger(point.character) || point.character < 0) {
        throw new Error('Invalid test source range');
      }
    }
    if (test.range.end.line < test.range.start.line
        || (test.range.end.line === test.range.start.line
            && test.range.end.character < test.range.start.character)) {
      throw new Error('Reversed test source range');
    }
  }
  return listing.tests;
}

/** Lists compiled tests incrementally and runs selections from the last build. */
export function activateTestExplorer(
  context: Pick<vscode.ExtensionContext, 'subscriptions'>,
  client: LanguageClient,
  env: NodeJS.ProcessEnv,
  workspaceFolder: string | undefined,
  synchronizeConfiguration: () => Promise<void>,
  getConfigPaths: () => string[]
): void {
  const controller = vscode.tests.createTestController('sun', 'Sun Tests');
  const output = vscode.window.createOutputChannel('Sun Tests');
  const targets = new WeakMap<vscode.TestItem, TestTarget>();
  const entrypointRuns = new Map<string, EntrypointRun>();
  const cache = new Map<string, { identity: string; tests: BinaryTest[] }>();
  const activeRunners = new Set<() => void>();
  const activeListings = new Set<() => void>();
  let disposed = false;
  let refreshGeneration = 0;
  let refreshTask: Promise<void> | undefined;
  let forceListing = false;
  let needsConfiguration = false;
  let debounce: NodeJS.Timeout | undefined;
  let watchers: vscode.FileSystemWatcher[] = [];
  let watchedKey = '';
  const polledBinaries = new Map<string, () => void>();
  context.subscriptions.push(controller, output);

  // Share the limit across overlapping editor run requests.
  const runnerLimit = Math.max(1, Math.floor(
    (os.availableParallelism?.() ?? os.cpus().length) / 2));
  let runningTests = 0;
  const queuedTests: Array<() => void> = [];

  /** Executes a test in a bounded process slot, including across editor runs. */
  function queueTest(action: () => Promise<void>, token: vscode.CancellationToken): Promise<void> {
    return new Promise((resolve, reject) => {
      /** Starts this test and releases its slot even when spawning fails. */
      const start = () => {
        cancellation.dispose();
        runningTests++;
        void action().then(resolve, reject).finally(() => {
          runningTests--;
          queuedTests.shift()?.();
        });
      };
      const cancellation = token.onCancellationRequested(() => {
        const index = queuedTests.indexOf(start);
        if (index >= 0) {
          queuedTests.splice(index, 1);
          cancellation.dispose();
          // The action observes cancellation and marks the test skipped.
          void action().then(resolve, reject);
        }
      });
      if (runningTests < runnerLimit) start();
      else queuedTests.push(start);
    });
  }

  /** Shows a suite relative to the workspace. */
  const entrypointLabel = (entrypoint: string): string =>
    workspaceFolder ? path.relative(workspaceFolder, entrypoint) : entrypoint;

  /** Describes results built from inputs that may differ from the editor. */
  function warning(suite: Suite): string | undefined {
    if (suite.freshness === 'stale') return 'Needs rebuild — tests from last build';
    if (suite.freshness === 'unknown') return 'Freshness unknown — tests from last build';
    return undefined;
  }

  /** Coalesces save and watcher events before requesting another inventory. */
  function scheduleRefresh(): void {
    if (disposed) return;
    if (debounce) clearTimeout(debounce);
    debounce = setTimeout(() => {
      debounce = undefined;
      void refreshWorkspace(false);
    }, 250);
  }

  /** Watches exact inputs and polls binaries under excluded build directories. */
  function updateWatchers(paths: string[], binaries: string[], patterns: string[] = []): void {
    const all = [...new Set([...getConfigPaths(), ...paths].map((p) => path.resolve(p)))].sort();
    const key = JSON.stringify([all, patterns]);
    if (key !== watchedKey) {
      watchedKey = key;
      for (const watcher of watchers) watcher.dispose();
      watchers = [];
      const byDirectory = new Map<string, Set<string>>();
      for (const file of all) {
        const directory = path.dirname(file);
        const files = byDirectory.get(directory) ?? new Set<string>();
        files.add(file);
        byDirectory.set(directory, files);
      }
      for (const [directory, files] of byDirectory) {
        const watcher = vscode.workspace.createFileSystemWatcher(
          new vscode.RelativePattern(vscode.Uri.file(directory), '*'));
        /** Refreshes only when a participating input changes. */
        const changed = (uri: vscode.Uri) => {
          if (files.has(path.resolve(uri.fsPath))) scheduleRefresh();
        };
        watcher.onDidCreate(changed);
        watcher.onDidChange(changed);
        watcher.onDidDelete(changed);
        watchers.push(watcher);
      }
      for (const pattern of patterns) {
        const wildcard = pattern.search(/[*?[]/);
        const slash = pattern.lastIndexOf('/', wildcard);
        if (wildcard < 0 || slash < 0) continue;
        const watcher = vscode.workspace.createFileSystemWatcher(new vscode.RelativePattern(
          vscode.Uri.file(pattern.slice(0, slash) || '/'), pattern.slice(slash + 1)));
        watcher.onDidCreate(scheduleRefresh);
        watcher.onDidChange(scheduleRefresh);
        watcher.onDidDelete(scheduleRefresh);
        watchers.push(watcher);
      }
    }
    const wanted = new Set(binaries);
    for (const [file, listener] of polledBinaries) {
      if (!wanted.has(file)) { fs.unwatchFile(file, listener); polledBinaries.delete(file); }
    }
    for (const file of wanted) {
      if (polledBinaries.has(file)) continue;
      /** Refreshes when a binary appears, disappears, or is replaced. */
      const listener = () => scheduleRefresh();
      fs.watchFile(file, { persistent: false, interval: 1000 }, listener);
      polledBinaries.set(file, listener);
    }
  }

  /** Lists a binary only after its embedded record advertises this protocol. */
  function listBinary(suite: Suite): Promise<BinaryTest[]> {
    return new Promise((resolve, reject) => {
      const child = spawn(suite.test_binary!, ['--list-tests'], {
        cwd: workspaceFolder ?? path.dirname(suite.entrypoint), env,
        detached: process.platform !== 'win32',
      });
      let stdout = '', stderr = '', done = false;
      const decoder = new StringDecoder('utf8');
      /** Terminates the listing and any processes it may have spawned. */
      const kill = () => {
        try {
          if (child.pid !== undefined && process.platform !== 'win32') process.kill(-child.pid, 'SIGKILL');
          else child.kill('SIGKILL');
        } catch { /* The process may already have exited. */ }
      };
      /** Settles once and releases the timeout and cancellation hook. */
      const finish = (error?: Error, tests?: BinaryTest[]) => {
        if (done) return;
        done = true;
        clearTimeout(timer);
        activeListings.delete(cancel);
        if (error) { kill(); reject(error); } else resolve(tests!);
      };
      /** Cancels listing when the extension stops. */
      const cancel = () => finish(new Error('Test listing cancelled'));
      const timer = setTimeout(() => finish(new Error('Test listing timed out after 10 seconds')), 10000);
      activeListings.add(cancel);
      child.stdout.on('data', (chunk: Buffer) => {
        stdout += decoder.write(chunk);
        if (stdout.length > 16 * 1024 * 1024) finish(new Error('Test listing exceeded output limit'));
      });
      child.stderr.on('data', (chunk: Buffer) => { stderr = (stderr + chunk.toString()).slice(-8192); });
      child.on('error', (error) => finish(error));
      child.on('close', (code, signal) => {
        if (done) return;
        if (code !== 0) { finish(new Error(`Test listing failed (${signal ?? code}): ${stderr}`)); return; }
        try { finish(undefined, parseListing(stdout + decoder.end())); } catch (error) { finish(new Error(String(error))); }
      });
    });
  }

  /** Replaces one suite's children using locations captured at build time. */
  function publishTests(root: vscode.TestItem, suite: Suite, tests: BinaryTest[]): void {
    const files = new Map<string, vscode.TestItem>();
    for (const test of tests) {
      const fileKey = test.file || suite.entrypoint;
      let file = files.get(fileKey);
      if (!file) {
        file = controller.createTestItem(`${suite.entrypoint}::file::${fileKey}`,
          path.basename(fileKey), vscode.Uri.file(fileKey));
        targets.set(file, { entrypoint: suite.entrypoint, dottedName: '' });
        files.set(fileKey, file);
      }
      const item = controller.createTestItem(`${suite.entrypoint}::${test.id}`, test.label,
        test.file ? vscode.Uri.file(test.file) : undefined);
      item.range = new vscode.Range(test.range.start.line, test.range.start.character,
        test.range.end.line, test.range.end.character);
      targets.set(item, { entrypoint: suite.entrypoint, dottedName: test.id });
      file.children.add(item);
    }
    root.children.replace([...files.values()]);
  }

  /** Publishes roots first, then fills them with at most two listing processes. */
  async function discoverWorkspace(generation: number, force: boolean): Promise<void> {
    const started = Date.now();
    if (!client.initializeResult?.capabilities.experimental?.sunTestSuites) {
      throw new Error('Update sun-lsp to enable compiled test discovery');
    }
    output.appendLine('Discovering compiled test suites...');
    const response = await client.sendRequest<SuiteResponse>('sun/testSuites', {});
    if (disposed || generation !== refreshGeneration) return;
    for (const error of response.errors) output.appendLine(error);
    updateWatchers(response.watched_paths,
      response.watched_binaries ?? response.suites.flatMap((suite) => suite.test_binary ? [suite.test_binary] : []),
      response.watched_patterns);
    const roots = response.suites.map((suite) => {
      const root = controller.createTestItem(suite.entrypoint, entrypointLabel(suite.entrypoint),
        vscode.Uri.file(suite.entrypoint));
      targets.set(root, { entrypoint: suite.entrypoint, dottedName: '' });
      root.description = warning(suite);
      if (suite.freshness === 'missing') root.description = 'Build required';
      else if (suite.listing_version !== 1) root.description = 'Rebuild required for test discovery';
      root.busy = suite.freshness !== 'missing' && suite.listing_version === 1;
      if (suite.freshness !== 'fresh') output.appendLine(`${entrypointLabel(suite.entrypoint)}: ${suite.reason}`);
      return root;
    });
    entrypointRuns.clear();
    controller.items.replace(roots);
    const binaries = new Set(response.suites.map((suite) => suite.test_binary));
    for (const binary of cache.keys()) if (!binaries.has(binary)) cache.delete(binary);
    let next = 0, total = 0;
    /** Discovers one suite at a time, publishing only the current generation. */
    const worker = async () => {
      while (next < response.suites.length && !disposed && generation === refreshGeneration) {
        const index = next++;
        const suite = response.suites[index], root = roots[index];
        const run: EntrypointRun = { testBinary: suite.test_binary, suite };
        entrypointRuns.set(suite.entrypoint, run);
        if (suite.freshness === 'missing' || suite.listing_version !== 1 || !suite.test_binary) continue;
        try {
          const identity = binaryIdentity(suite.test_binary);
          if (identity !== suite.binary_identity) {
            scheduleRefresh();
            throw new Error('Test binary changed since inventory; refreshing');
          }
          const cached = cache.get(suite.test_binary);
          const tests = !force && cached?.identity === identity ? cached.tests : await listBinary(suite);
          if (disposed || generation !== refreshGeneration) return;
          if (binaryIdentity(suite.test_binary) !== identity) {
            scheduleRefresh();
            throw new Error('Test binary changed during discovery; refreshing');
          }
          run.identity = identity;
          cache.set(suite.test_binary, { identity, tests });
          publishTests(root, suite, tests);
          total += tests.length;
        } catch (error) {
          if (disposed || generation !== refreshGeneration) return;
          root.error = String(error);
          output.appendLine(`${entrypointLabel(suite.entrypoint)}: ${error}`);
        } finally { root.busy = false; }
      }
    };
    await Promise.all([worker(), worker()]);
    output.appendLine(`Discovery: ${response.suites.length} suites, ${total} tests in ${Date.now() - started} ms`);
  }

  /** Serializes inventories and combines newer requests into one follow-up pass. */
  function refreshWorkspace(force: boolean): Promise<void> {
    if (disposed) return Promise.resolve();
    if (debounce) { clearTimeout(debounce); debounce = undefined; }
    forceListing ||= force;
    ++refreshGeneration;
    for (const cancel of activeListings) cancel();
    if (!refreshTask) {
      refreshTask = (async () => {
        let generation: number;
        do {
          generation = refreshGeneration;
          const force = forceListing;
          forceListing = false;
          try {
            if (needsConfiguration) {
              needsConfiguration = false;
              await synchronizeConfiguration();
            }
            if (!disposed) await discoverWorkspace(generation, force);
          } catch (error) {
            if (!disposed && generation === refreshGeneration) {
              output.appendLine(`Test discovery failed: ${error}`);
              const root = controller.createTestItem('sun-discovery-error', 'Sun test discovery');
              root.error = String(error);
              controller.items.replace([root]);
            }
          }
        } while (!disposed && generation !== refreshGeneration);
      })().finally(() => { refreshTask = undefined; });
    }
    return refreshTask;
  }

  controller.refreshHandler = () => refreshWorkspace(true);
  updateWatchers([], []);
  const sourceWatcher = vscode.workspace.createFileSystemWatcher('**/*.sun');
  sourceWatcher.onDidCreate(scheduleRefresh);
  sourceWatcher.onDidChange(scheduleRefresh);
  sourceWatcher.onDidDelete(scheduleRefresh);
  context.subscriptions.push(sourceWatcher,
    vscode.workspace.onDidSaveTextDocument((document) => {
      if (document.languageId === 'sun' && document.uri.scheme === 'file') scheduleRefresh();
    }),
    vscode.workspace.onDidChangeTextDocument((event) => {
      if (event.document.languageId === 'sun' && event.document.uri.scheme === 'file') scheduleRefresh();
    }),
    vscode.workspace.onDidCloseTextDocument((document) => {
      if (document.languageId === 'sun') scheduleRefresh();
    }),
    vscode.workspace.onDidChangeConfiguration((event) => {
      if (['sun.entrypoints', 'sun.sun_configs', 'sun.path_variables'].some((key) => event.affectsConfiguration(key))) {
        needsConfiguration = true;
        scheduleRefresh();
      }
    }),
    { dispose: () => {
      disposed = true;
      if (debounce) clearTimeout(debounce);
      for (const cancel of activeListings) cancel();
      for (const cancel of activeRunners) cancel();
      for (const watcher of watchers) watcher.dispose();
      for (const [file, listener] of polledBinaries) fs.unwatchFile(file, listener);
    } });

  /** Leaf test items under an item (a leaf returns itself). */
  function leavesOf(item: vscode.TestItem): vscode.TestItem[] {
    if (item.children.size === 0) {
      return [item];
    }
    const leaves: vscode.TestItem[] = [];
    item.children.forEach((child) => leaves.push(...leavesOf(child)));
    return leaves;
  }

  /** Runs one isolated test and reports success only after its process exits cleanly. */
  function executeTest(command: string, entrypoint: string, name: string,
    item: vscode.TestItem, run: vscode.TestRun, token: vscode.CancellationToken): Promise<void> {
    const args = ['--test-sequential', '--test-filter', name];
    output.appendLine(`> ${command} ${args.join(' ')}`);
    return new Promise((resolve) => {
      const child = spawn(command, args, {
        cwd: workspaceFolder ?? path.dirname(entrypoint), env,
        detached: process.platform !== 'win32',
      });
      let done = false, passed = false;
      let failure: string | undefined;
      let buffered = '', diagnosticOutput = '';
      const stdout = new StringDecoder('utf8'), stderr = new StringDecoder('utf8');
      let killTimer: NodeJS.Timeout | undefined;
      /** Signals the test process and its children when the run is cancelled. */
      const signalRunner = (signal: NodeJS.Signals) => {
        if (child.exitCode !== null || child.signalCode !== null) return;
        try {
          if (child.pid !== undefined && process.platform !== 'win32') process.kill(-child.pid, signal);
          else child.kill(signal);
        } catch { /* The process may already have exited. */ }
      };
      /** Stops a runner on cancellation or extension disposal. */
      const cancelRunner = () => {
        if (done || killTimer) return;
        signalRunner('SIGTERM');
        killTimer = setTimeout(() => signalRunner('SIGKILL'), 2000);
      };
      activeRunners.add(cancelRunner);
      const cancellation = token.onCancellationRequested(cancelRunner);
      /** Settles once, releasing cancellation and timeout resources. */
      const finish = (report: () => void) => {
        if (done) return;
        done = true;
        if (killTimer) clearTimeout(killTimer);
        cancellation.dispose();
        activeRunners.delete(cancelRunner);
        report();
        resolve();
      };
      /** Streams output to the editor while retaining a bounded diagnostic tail. */
      const appendOutput = (text: string) => {
        diagnosticOutput = (diagnosticOutput + text).slice(-8192);
        run.appendOutput(text.replace(/\r?\n/g, '\r\n'), undefined, item);
      };
      /** Records the selected test's result without accepting output from other names. */
      const handleLine = (line: string) => {
        if (line === `PASS ${name}`) passed = true;
        const prefix = `FAIL ${name}:`;
        if (line.startsWith(prefix)) failure = line.slice(prefix.length).trimStart() || 'test failed';
      };
      /** Decodes complete lines without corrupting UTF-8 split across data events. */
      const consume = (text: string) => {
        if (done) return;
        buffered += text;
        let newline;
        while ((newline = buffered.indexOf('\n')) >= 0) {
          const line = buffered.slice(0, newline).replace(/\r$/, '');
          appendOutput(line + '\n');
          handleLine(line);
          buffered = buffered.slice(newline + 1);
        }
      };
      child.stdout.on('data', (chunk: Buffer) => consume(stdout.write(chunk)));
      child.stderr.on('data', (chunk: Buffer) => { if (!done) appendOutput(stderr.write(chunk)); });
      child.on('error', (error) => finish(() => run.errored(item, new vscode.TestMessage(String(error)))));
      // Cancellation must not wait on output pipes held by a surviving child.
      child.on('exit', () => {
        if (disposed || token.isCancellationRequested) finish(() => run.skipped(item));
      });
      child.on('close', (code, signal) => {
        if (done) return;
        consume(stdout.end());
        appendOutput(stderr.end());
        if (buffered) {
          appendOutput(buffered);
          handleLine(buffered.replace(/\r$/, ''));
        }
        finish(() => {
          if (disposed || token.isCancellationRequested) run.skipped(item);
          else if (!signal && failure !== undefined && code === 1) {
            run.failed(item, new vscode.TestMessage(failure));
          } else if (!signal && code === 0 && passed && failure === undefined) run.passed(item);
          else {
            const reason = signal ? `the test runner was killed by ${signal}`
              : code !== 0 ? `the test runner exited with code ${code}`
              : 'the test runner reported no valid result';
            run.errored(item, new vscode.TestMessage(`${reason}\n\n${diagnosticOutput.trim()}`.trimEnd()));
          }
        });
      });
    });
  }

  /** Runs the selected Sun tests and reports their progress to the editor. */
  async function runTests(
    request: vscode.TestRunRequest,
    token: vscode.CancellationToken
  ): Promise<void> {
    if (disposed || token.isCancellationRequested) return;
    const run = controller.createTestRun(request);
    const queued = new Map<string, vscode.TestItem>();
    /** Shows waiting tests immediately, keeping refreshed items under the same ID. */
    const enqueue = (item: vscode.TestItem) => {
      if (!queued.has(item.id)) run.enqueued(item);
      queued.set(item.id, item);
    };
    /** Resolves a queued or running test with an actionable error. */
    const reportError = (item: vscode.TestItem, message: vscode.TestMessage) => {
      queued.delete(item.id);
      run.errored(item, message);
    };
    /** Clears a waiting test when execution is cancelled or no longer selected. */
    const skip = (item: vscode.TestItem) => {
      queued.delete(item.id);
      run.skipped(item);
    };
    try {
      const initialExcluded = new Set((request.exclude ?? []).flatMap((item) =>
        leavesOf(item).map((leaf) => leaf.id)));
      const initialRoots = request.include ?? [...controller.items].map(([, item]) => item);
      for (const root of initialRoots) {
        for (const item of leavesOf(root)) {
          if (!initialExcluded.has(item.id)) enqueue(item);
        }
      }
      await refreshWorkspace(false);
      if (disposed || token.isCancellationRequested) return;
      const current = new Map<string, vscode.TestItem>();
      /** Resolves old UI selections against the refreshed binary listing. */
      const index = (item: vscode.TestItem) => {
        current.set(item.id, item);
        item.children.forEach(index);
      };
      controller.items.forEach(index);
      // Collect the requested leaves, grouped by entrypoint.
      const requested: vscode.TestItem[] = [];
      if (request.include) {
        for (const item of request.include) {
          const refreshed = current.get(item.id);
          if (refreshed) requested.push(...leavesOf(refreshed));
          else reportError(item, new vscode.TestMessage('Test no longer exists in the current binary'));
        }
      } else {
        controller.items.forEach((root) => requested.push(...leavesOf(root)));
      }
      const excluded = new Set((request.exclude ?? []).flatMap((item) =>
        leavesOf(current.get(item.id) ?? item).map((leaf) => leaf.id)));
      const byEntrypoint = new Map<string, vscode.TestItem[]>();
      for (const item of new Set(requested)) {
        if (excluded.has(item.id)) continue;
        enqueue(item);
        const target = targets.get(item);
        if (!target) continue;
        if (!target.dottedName) {
          const info = entrypointRuns.get(target.entrypoint);
          if (info?.suite.freshness === 'missing' || info?.suite.listing_version !== 1 || item.error) {
            reportError(item, new vscode.TestMessage(String(item.error ?? item.description ?? 'Build required')));
          }
          continue;
        }
        const group = byEntrypoint.get(target.entrypoint) ?? [];
        group.push(item);
        byEntrypoint.set(target.entrypoint, group);
      }

      // A rebuild can remove leaves even when their suite is still selected.
      const selectedIds = new Set(requested.map((item) => item.id));
      for (const item of [...queued.values()]) {
        if (!selectedIds.has(item.id)) {
          if (current.get(item.id)?.children.size) skip(item);
          else reportError(item, new vscode.TestMessage('Test no longer exists in the current binary'));
        }
      }

      for (const [entrypoint, items] of byEntrypoint) {
        if (disposed || token.isCancellationRequested) break;

        const root = controller.items.get(entrypoint);
        const runInfo = entrypointRuns.get(entrypoint);
        try {
          const inventory = await client.sendRequest<SuiteResponse>('sun/testSuites', {});
          const latest = inventory.suites.find((suite) => suite.entrypoint === entrypoint);
          if (!latest || !runInfo || latest.binary_identity !== runInfo.identity
              || latest.test_binary !== runInfo.testBinary || latest.listing_version !== 1) {
            for (const item of items) reportError(item, new vscode.TestMessage('Suite changed; refresh discovery before running'));
            scheduleRefresh();
            continue;
          }
          runInfo.suite = latest;
          if (root) root.description = warning(latest);
        } catch (error) {
          if (runInfo) runInfo.suite = { ...runInfo.suite, freshness: 'unknown', reason: String(error) };
          if (root && runInfo) root.description = warning(runInfo.suite);
        }
        if (disposed || token.isCancellationRequested) break;
        let buildError: string | undefined;
        try {
          if (!runInfo?.testBinary || runInfo.suite.freshness === 'missing') buildError = 'Build required';
          else if (!runInfo.identity || binaryIdentity(runInfo.testBinary) !== runInfo.identity) {
            buildError = 'Test binary changed; refresh discovery before running';
            scheduleRefresh();
          }
        } catch (error) { buildError = String(error); }
        if (buildError) {
          for (const item of items) reportError(item, new vscode.TestMessage(buildError));
          continue;
        }
        const staleWarning = warning(runInfo!.suite);
        if (staleWarning) {
          const message = `${entrypointLabel(entrypoint)}: ${staleWarning}. ${runInfo!.suite.reason}`;
          output.appendLine(message);
          run.appendOutput(message + '\r\n');
        }
        const command = runInfo!.testBinary!;
        await Promise.all(items.map((item) => queueTest(async () => {
          if (disposed || token.isCancellationRequested) {
            skip(item);
            return;
          }
          // A rebuild may happen while this test waits for a process slot.
          try {
            if (binaryIdentity(command) !== runInfo!.identity) {
              throw new Error('Test binary changed; refresh discovery before running');
            }
          } catch (error) {
            reportError(item, new vscode.TestMessage(String(error)));
            scheduleRefresh();
            return;
          }
          const name = targets.get(item)!.dottedName;
          queued.delete(item.id);
          run.started(item);
          await executeTest(command, entrypoint, name, item, run, token);
        }, token)));
      }
    } finally {
      // Whatever happened above, the run must end, or every test it started
      // stays spinning in the explorer.
      for (const item of [...queued.values()]) skip(item);
      run.end();
    }
  }

  controller.createRunProfile(
    'Run',
    vscode.TestRunProfileKind.Run,
    runTests,
    true
  );

  void refreshWorkspace(false);
}
