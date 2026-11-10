/** Exercises binary discovery, freshness warnings, and language-service lifecycle. */
const assert = require('node:assert/strict');
const { EventEmitter } = require('node:events');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const { test } = require('node:test');

/** Supplies a disposable event source for editor mocks. */
function event() {
  const listeners = new Set();
  return {
    /** Registers an event callback. */
    listen(fn) { listeners.add(fn); return { dispose() { listeners.delete(fn); } }; },
    /** Delivers an event to current listeners. */
    fire(value) { for (const fn of [...listeners]) fn(value); },
  };
}
/** Models the editor's test-item collection. */
class Items extends Map {
  /** Inserts a test item by its stable ID. */
  add(item) { this.set(item.id, item); }
  /** Replaces the displayed children. */
  replace(items) { this.clear(); for (const item of items) this.add(item); }
}
/** Loads compiled extension code with controlled editor and process dependencies. */
function load(name, mocks) {
  const filename = path.join(__dirname, '../out', name + '.js');
  const exports = {};
  vm.runInNewContext(fs.readFileSync(filename, 'utf8'), {
    exports, process, Buffer, setTimeout, clearTimeout,
    require: (id) => id in mocks ? mocks[id] : require(id),
  }, { filename });
  return exports;
}
/** Flushes promise and process callbacks. */
const flush = async () => { for (let i = 0; i < 5; ++i) await new Promise(setImmediate); };
/** Waits for the automatic refresh debounce. */
const debounce = () => new Promise((resolve) => setTimeout(resolve, 300));
/** Returns the versioned listing emitted by a compiled test binary. */
const binaryListing = () => ({ version: 1, tests: ['first', 'second'].map((name) => ({
  id: 'suite.' + name, label: name, file: '/workspace/tests.sun',
  range: { start: { line: 0, character: 0 }, end: { line: 0, character: 1 } },
})) });
/** Returns an inventory with supported, fresh binaries. */
const listing = (...entries) => ({ suites: entries.map((entrypoint) => ({
  entrypoint, test_binary: entrypoint + '_test', binary_identity: '1:2:3:4:5',
  listing_version: 1, freshness: 'fresh', reason: 'Inputs unchanged', sources: [entrypoint],
})), watched_paths: ['/outside/custom.json'], errors: [] });
/** Provides observable editor state and child processes without executing binaries. */
function harness(fileSystem = {}) {
  const saved = event(), changed = event(), closed = event(), settingsChanged = event();
  const watchers = [], settings = {}, calls = [], commands = [], children = [], logs = [], runs = [];
  const polled = new Map();
  const controller = {
    items: new Items(),
    /** Creates an item with a mutable child collection. */
    createTestItem: (id, label, uri) => ({ id, label, uri, children: new Items() }),
    /** Exposes the run callback to tests. */
    createRunProfile(_name, _kind, run) { this.run = run; },
    /** Records runner events and output. */
    createTestRun() {
      const result = { enqueued: [], started: [], passed: [], failed: [], errors: [], output: [], skipped: [], ended: false };
      runs.push(result);
      return {
        /** Records immediate feedback before discovery or a process slot is ready. */
        enqueued(item) { result.enqueued.push(item.id); },
        /** Records a test beginning execution. */
        started(item) { result.started.push(item.id); },
        /** Records a successful runner result. */
        passed(item) { result.passed.push(item.id); },
        /** Records an assertion failure without affecting other processes. */
        failed(item) { result.failed.push(item.id); },
        /** Records tests interrupted before completion. */
        skipped(item) { result.skipped.push(item.id); },
        /** Records actionable errors shown on test items. */
        errored(item, message) { result.errors.push(message.message); },
        /** Collects warnings and runner output. */
        appendOutput(text) { result.output.push(text); },
        /** Records that the editor run settled. */
        end() { result.ended = true; },
      };
    },
    /** Models controller disposal. */
    dispose() {},
  };
  /** Creates a file URI for editor APIs. */
  const uri = (file) => ({ fsPath: file, scheme: 'file', toString: () => 'file://' + file });
  const vscode = {
    Uri: { file: uri },
    /** Stands in for an editor source range. */
    Range: class {},
    /** Retains the base directory of a file watcher. */
    RelativePattern: class { /** Stores watcher arguments. */ constructor(base, pattern) { this.baseUri = base; this.pattern = pattern; } },
    TestRunProfileKind: { Run: 1 },
    /** Retains errors reported to the editor. */
    TestMessage: class { /** Stores the displayed error. */ constructor(message) { this.message = message; } },
    tests: { createTestController: (_id, label) => { controller.label = label; return controller; } },
    workspace: {
      workspaceFolders: [{ uri: uri('/workspace') }],
      getConfiguration: () => ({ get: (key, fallback) => settings[key] ?? fallback }),
      onDidSaveTextDocument: saved.listen, onDidChangeTextDocument: changed.listen,
      onDidCloseTextDocument: closed.listen, onDidChangeConfiguration: settingsChanged.listen,
      /** Registers observable file events. */
      createFileSystemWatcher(pattern) {
        const create = event(), change = event(), remove = event();
        const watcher = {
          pattern, create, change, remove, disposed: false,
          onDidCreate: create.listen, onDidChange: change.listen, onDidDelete: remove.listen,
          /** Records watcher cleanup. */
          dispose() { this.disposed = true; },
        };
        watchers.push(watcher);
        return watcher;
      },
    },
    window: { createOutputChannel: () => ({ appendLine(line) { logs.push(line); }, dispose() {} }) },
  };
  const client = {
    initializeResult: { capabilities: { experimental: { sunTestSuites: true } } },
    /** Returns the current suite inventory. */
    async sendRequest(method) { calls.push(method); return client.response; },
    response: listing(),
  };
  const options = { holdListings: false, holdRuns: false, listingText: undefined, identity: 5n };
  const mocks = {
    vscode, 'vscode-languageclient/node': {},
    'node:os': { availableParallelism: () => 4 },
    'node:fs': {
      ...fs,
      /** Supplies a controllable binary replacement identity. */
      statSync: () => ({ dev: 1n, ino: 2n, size: 3n, mtimeNs: 4n, ctimeNs: options.identity, isFile: () => true }),
      /** Records polling of excluded binary paths. */
      watchFile(file, _options, listener) { polled.set(file, listener); },
      /** Removes a polled binary on disposal. */
      unwatchFile(file) { polled.delete(file); },
      ...fileSystem,
    },
    'node:child_process': {
      /** Models listing and runner processes with controllable completion. */
      spawn(command, args) {
        commands.push({ command, args: [...args] });
        const child = new EventEmitter();
        child.stdout = new EventEmitter(); child.stderr = new EventEmitter();
        child.exitCode = null; child.signalCode = null; child.signals = [];
        /** Records signals and delivers process shutdown events. */
        child.kill = (signal) => {
          child.signals.push(signal); child.signalCode = signal;
          setImmediate(() => { child.emit('exit', null, signal); child.emit('close', null, signal); });
        };
        children.push(child);
        const isListing = args[0] === '--list-tests';
        /** Delivers complete or fragmented runner output. */
        child.complete = (text = isListing ? options.listingText ?? JSON.stringify(binaryListing()) : `PASS ${args[args.indexOf('--test-filter') + 1]}\n`, code = 0) => {
          child.stdout.emit('data', Buffer.from(text)); child.exitCode = code; child.emit('close', code);
        };
        if (!(isListing ? options.holdListings : options.holdRuns)) setImmediate(() => child.complete());
        return child;
      },
    },
  };
  const context = { subscriptions: [] };
  /** Activates discovery with the mocked services. */
  const start = () => load('testExplorer', mocks).activateTestExplorer(
    context, client, {}, '/workspace', async () => { calls.push('settings'); },
    () => settings.sun_configs ?? ['/outside/custom.json']);
  return { saved, changed, closed, settingsChanged, watchers, settings, calls, commands, children,
    controller, uri, client, mocks, start, logs, runs, options, polled,
    dispose: () => context.subscriptions.forEach((d) => d.dispose()) };
}
/** Supplies a non-cancelled run token. */
const token = () => ({ isCancellationRequested: false, onCancellationRequested: () => ({ dispose() {} }) });

/** Suites appear before their tests, and only two binaries are queried at once. */
test('incremental discovery is bounded and shared files retain independent suites', async () => {
  const h = harness(); h.options.holdListings = true;
  h.client.response = listing('/workspace/one.sun', '/workspace/two.sun', '/workspace/three.sun');
  h.start(); await flush();
  assert.equal(h.controller.items.size, 3); assert.equal(h.commands.length, 2);
  assert.equal(h.controller.items.get('/workspace/one.sun').children.size, 0);
  h.children[0].complete(); await flush();
  assert.equal(h.controller.items.get('/workspace/one.sun').children.size, 1);
  assert.equal(h.commands.length, 3);
  h.children[1].complete(); h.children[2].complete(); await flush();
  assert.equal(new Set([...h.controller.items.values()].map((r) => [...r.children.keys()][0])).size, 3);
  h.dispose();
});

/** Source edits update freshness using cached listings; manual refresh lists again. */
test('source and watcher bursts coalesce and preserve the binary cache', async () => {
  const h = harness(); h.client.response = listing('/workspace/main.sun');
  h.start(); await flush();
  h.client.response.suites[0].freshness = 'stale';
  h.saved.fire({ languageId: 'sun', uri: h.uri('/workspace/main.sun') });
  h.changed.fire({ document: { languageId: 'sun', uri: h.uri('/workspace/main.sun') } });
  h.watchers[0].change.fire(h.uri('/outside/custom.json'));
  await debounce(); await flush();
  assert.equal(h.commands.length, 1); assert.equal(h.calls.length, 2);
  assert.match(h.controller.items.get('/workspace/main.sun').description, /Needs rebuild/);
  await h.controller.refreshHandler(); assert.equal(h.commands.length, 2);
  h.dispose(); assert.equal(h.polled.size, 0);
});

/** Staleness never blocks execution or disappears after a passing run. */
test('stale and unknown binaries run with warnings and retain their status', async () => {
  for (const freshness of ['stale', 'unknown']) {
    const h = harness(); h.client.response = listing('/workspace/main.sun');
    h.client.response.suites[0].freshness = freshness;
    h.start(); await flush();
    const root = h.controller.items.get('/workspace/main.sun');
    const leaf = [...[...root.children.values()][0].children.values()][0];
    await h.controller.run({ include: [leaf] }, token());
    assert.deepEqual(h.commands.at(-1).args, ['--test-sequential', '--test-filter', 'suite.first']);
    assert.equal(h.runs[0].errors.length, 0); assert.equal(h.runs[0].ended, true);
    assert.match(h.runs[0].output.join(''), /tests from last build/);
    assert.match(h.controller.items.get(root.id).description, /tests from last build/);
    h.dispose();
  }
});

/** Unsupported or missing artifacts must never be probed by execution. */
test('legacy, missing, and replaced binaries are not launched for discovery', async () => {
  for (const scenario of ['legacy', 'missing', 'replaced']) {
    const h = harness(); h.client.response = listing('/workspace/main.sun');
    const suite = h.client.response.suites[0];
    if (scenario === 'legacy') suite.listing_version = 0;
    if (scenario === 'missing') suite.freshness = 'missing';
    if (scenario === 'replaced') h.options.identity = 6n;
    h.start(); await flush();
    assert.equal(h.commands.length, 0, scenario);
    const root = h.controller.items.get(suite.entrypoint);
    assert.match(root.error ?? root.description, /[Bb]uild|changed/);
    h.dispose();
  }
});

/** A rebuild invalidates the cache and selections use the updated listing. */
test('binary replacement relists before executing a previous selection', async () => {
  const h = harness(); h.client.response = listing('/workspace/main.sun');
  h.start(); await flush();
  const root = h.controller.items.get('/workspace/main.sun');
  h.options.identity = 6n; h.client.response.suites[0].binary_identity = '1:2:3:4:6';
  await h.controller.run({ include: [root] }, token());
  assert.deepEqual(h.commands.map((c) => c.args), [['--list-tests'], ['--list-tests'],
    ['--test-sequential', '--test-filter', 'suite.first'],
    ['--test-sequential', '--test-filter', 'suite.second']]);
  h.dispose();
});

/** Listing errors affect one suite and do not hide healthy siblings. */
test('invalid listing and nonzero exit remain isolated to their suites', async () => {
  const h = harness(); h.options.holdListings = true;
  h.client.response = listing('/workspace/bad.sun', '/workspace/good.sun');
  h.start(); await flush();
  h.children[0].complete('{bad json'); h.children[1].complete(); await flush();
  assert.ok(h.controller.items.get('/workspace/bad.sun').error);
  assert.equal(h.controller.items.get('/workspace/good.sun').children.size, 1);
  h.dispose();
});

/** Obsolete work never publishes after a newer request or disposal. */
test('overlapping refreshes discard obsolete listings and remove deleted suites', async () => {
  const h = harness(); h.options.holdListings = true;
  h.client.response = listing('/workspace/old.sun'); h.start(); await flush();
  h.client.response = listing('/workspace/new.sun');
  const refreshing = h.controller.refreshHandler();
  h.children[0].complete(); await flush();
  assert.deepEqual([...h.controller.items.keys()], ['/workspace/new.sun']);
  h.dispose(); await refreshing;
  assert.deepEqual(h.children[1].signals, ['SIGKILL']);
});

/** A listing timeout releases the queue and remains visible to the user. */
test('listing timeout terminates the process', async () => {
  const h = harness(); h.options.holdListings = true;
  h.client.response = listing('/workspace/main.sun'); h.start(); await flush();
  await new Promise((resolve) => setTimeout(resolve, 10100));
  assert.match(h.controller.items.get('/workspace/main.sun').error, /timed out/);
  assert.deepEqual(h.children[0].signals, ['SIGKILL']); h.dispose();
});

/** Older servers produce an actionable error without invoking old source discovery. */
test('missing inventory capability reports the required LSP update', async () => {
  const h = harness(); h.client.initializeResult.capabilities.experimental = {};
  h.start(); await flush();
  assert.match([...h.controller.items.values()][0].error, /Update sun-lsp/);
  assert.equal(h.calls.length, 0); h.dispose();
});

/** Failed inventories can be retried and settings are sent only when changed. */
test('configuration changes and inventory recovery keep exact config watchers', async () => {
  const h = harness(); h.client.sendRequest = async () => { throw new Error('unavailable'); };
  h.start(); await flush();
  h.client.sendRequest = async () => listing('/workspace/main.sun');
  h.settings.sun_configs = ['/different/project.json'];
  h.settingsChanged.fire({ affectsConfiguration: (key) => key === 'sun.sun_configs' });
  await debounce(); await flush();
  assert.deepEqual(h.calls, ['settings']);
  assert.ok(h.watchers.some((w) => w.pattern.baseUri?.fsPath === '/different'));
  assert.equal(h.controller.items.get('/workspace/main.sun').children.size, 1); h.dispose();
});

/** Disposal cancels a running suite and prevents the next suite starting. */
test('disposing stops the active runner and queued suites', async () => {
  const h = harness(); h.client.response = listing('/workspace/one.sun', '/workspace/two.sun');
  h.start(); await flush(); h.options.holdRuns = true;
  const running = h.controller.run({}, token()); await flush(); h.dispose(); await running;
  assert.deepEqual(h.children.at(-1).signals, ['SIGTERM']);
  assert.equal(h.commands.filter((c) => c.args[0] !== '--list-tests').length, 2);
  assert.ok(h.children.slice(2).every((child) => child.signals.includes('SIGTERM')));
  assert.equal(h.runs[0].ended, true);
});
/** Supplies observable language services for activation and live-toggle tests. */
function languageServicesHarness(h, start = async () => {}) {
  const clients = [];
  let explorerStarts = 0;
  let explorerDisposals = 0;
  h.settings.lsp_path = process.execPath;
  h.mocks['vscode-languageclient/node'] = {
    TransportKind: { stdio: 0 },
    /** Records service startup and shutdown without launching a server. */
    LanguageClient: class {
      /** Tracks one independent language-client instance. */
      constructor() { this.stopped = false; clients.push(this); }
      /** Allows tests to delay initialization while settings change. */
      async start() { await start(); }
      /** Records that all client-owned language features were stopped. */
      async stop() { this.stopped = true; }
    },
  };
  h.mocks['./testExplorer'] = {
    /** Registers a disposable standing in for all test-discovery resources. */
    activateTestExplorer(context) {
      explorerStarts++;
      context.subscriptions.push({
        /** Records cleanup when syntax-only mode is enabled. */
        dispose() { explorerDisposals++; },
      });
    },
  };
  const context = { subscriptions: [] };
  return {
    extension: load('extension', h.mocks), context, clients,
    /** Returns the number of test integrations created and disposed. */
    explorerCounts: () => [explorerStarts, explorerDisposals],
    /** Changes the live mode through the same event emitted by VS Code. */
    setSyntaxOnly(value) {
      h.settings.syntaxOnly = value;
      h.settingsChanged.fire({ affectsConfiguration: (name) => name === 'sun.syntaxOnly' });
    },
  };
}

/** Syntax-only startup must work without locating or launching compiler tools. */
test('syntax-only startup does not check tools or start language services', async () => {
  const h = harness({ existsSync() { throw new Error('must not check executables'); } });
  h.settings.syntaxOnly = true;
  const services = languageServicesHarness(h);
  await services.extension.activate(services.context);
  assert.equal(services.clients.length, 0);
  assert.deepEqual(services.explorerCounts(), [0, 0]);
  await services.extension.deactivate();
  services.context.subscriptions.forEach((subscription) => subscription.dispose());
});

/** Live toggles dispose services and restore them without duplicate clients. */
test('syntax-only mode stops and restores the language server and test explorer', async () => {
  const services = languageServicesHarness(harness());
  await services.extension.activate(services.context);
  assert.equal(services.clients.length, 1);
  services.setSyntaxOnly(true);
  await flush();
  assert.equal(services.clients[0].stopped, true);
  assert.deepEqual(services.explorerCounts(), [1, 1]);
  services.setSyntaxOnly(false);
  services.setSyntaxOnly(false);
  await flush();
  assert.equal(services.clients.length, 2);
  assert.equal(services.clients[1].stopped, false);
  assert.deepEqual(services.explorerCounts(), [2, 1]);
  await services.extension.deactivate();
  assert.equal(services.clients[1].stopped, true);
  assert.deepEqual(services.explorerCounts(), [2, 2]);
  services.context.subscriptions.forEach((subscription) => subscription.dispose());
});

/** Enabling syntax-only mode during startup must not activate test discovery. */
test('syntax-only mode requested during server startup stops it before discovery', async () => {
  let finishStartup;
  const services = languageServicesHarness(harness(), () => new Promise((resolve) => { finishStartup = resolve; }));
  const activation = services.extension.activate(services.context);
  await flush();
  services.setSyntaxOnly(true);
  finishStartup();
  await activation;
  await flush();
  assert.equal(services.clients.length, 1);
  assert.equal(services.clients[0].stopped, true);
  assert.deepEqual(services.explorerCounts(), [0, 0]);
  await services.extension.deactivate();
  services.context.subscriptions.forEach((subscription) => subscription.dispose());
});

/** JSON split inside a UTF-8 character must preserve the compiled source path. */
test('listing chunks preserve Unicode and reject unsupported versions', async () => {
  const h = harness(); h.options.holdListings = true;
  h.client.response = listing('/workspace/unicode.sun', '/workspace/unsupported.sun');
  h.start(); await flush();
  const payload = binaryListing(); payload.tests[0].file = '/workspace/😀.sun';
  const bytes = Buffer.from(JSON.stringify(payload));
  const split = bytes.indexOf(Buffer.from('😀')) + 1;
  h.children[0].stdout.emit('data', bytes.subarray(0, split));
  h.children[0].complete(bytes.subarray(split));
  h.children[1].complete(JSON.stringify({ version: 999, tests: [] }));
  await flush();
  const unicode = h.controller.items.get('/workspace/unicode.sun');
  const module = [...unicode.children.values()][0];
  assert.ok([...module.children.values()].some((item) => item.uri.fsPath === '/workspace/😀.sun'));
  assert.match(h.controller.items.get('/workspace/unsupported.sun').error, /Unsupported/);
  h.dispose();
});

/** One broken binary must not prevent run-all from using healthy suites. */
test('run all reports missing suites and still executes available tests', async () => {
  const h = harness(); h.client.response = listing('/workspace/missing.sun', '/workspace/ready.sun');
  h.client.response.suites[0].freshness = 'missing';
  h.start(); await flush(); await h.controller.run({}, token());
  assert.match(h.runs[0].errors[0], /Build required/);
  assert.equal(h.runs[0].passed.length, 2); assert.equal(h.runs[0].ended, true);
  h.dispose();
});

/** Deleted tests cannot run using names saved from the previous build. */
test('removed test selections report an error after a rebuild', async () => {
  const h = harness(); h.client.response = listing('/workspace/main.sun');
  h.start(); await flush();
  const leaf = [...[...h.controller.items.values()][0].children.values()][0].children.values().next().value;
  h.options.identity = 6n; h.client.response.suites[0].binary_identity = '1:2:3:4:6';
  h.options.listingText = JSON.stringify({ version: 1, tests: [] });
  await h.controller.run({ include: [leaf] }, token());
  assert.match(h.runs[0].errors[0], /no longer exists/);
  assert.ok(h.commands.every((command) => command.args[0] === '--list-tests'));
  h.dispose();
});

/** A failed obsolete inventory must not replace a newer tree with an error. */
test('obsolete inventory errors are ignored during a newer refresh', async () => {
  const h = harness();
  let rejectOld, requests = 0;
  h.client.sendRequest = async () => {
    requests++;
    if (requests === 1) return new Promise((_resolve, reject) => { rejectOld = reject; });
    return listing('/workspace/current.sun');
  };
  h.start(); await flush();
  const refreshed = h.controller.refreshHandler();
  rejectOld(new Error('obsolete failure')); await refreshed;
  assert.equal(h.controller.items.get('/workspace/current.sun').children.size, 1);
  assert.ok(!h.logs.some((line) => line.includes('obsolete failure')));
  h.dispose();
});

/** Isolated processes run in parallel without sharing signal handlers or descriptors. */
test('run all isolates tests, bounds concurrency, and attributes failures independently', async () => {
  const h = harness(); h.options.holdRuns = true;
  h.client.response = listing('/workspace/main.sun');
  const payload = binaryListing();
  payload.tests.push({ ...payload.tests[0], id: 'suite.third', label: 'third' });
  h.options.listingText = JSON.stringify(payload);
  h.start(); await flush();
  const running = h.controller.run({}, token()); await flush();
  assert.equal(h.children.length, 3); // One listing and two execution slots.
  assert.deepEqual(h.commands.slice(1).map((c) => c.args), [
    ['--test-sequential', '--test-filter', 'suite.first'],
    ['--test-sequential', '--test-filter', 'suite.second'],
  ]);
  h.children[1].complete('FAIL suite.first: deliberate failure\n', 1);
  await flush();
  assert.equal(h.children.length, 4);
  assert.equal(h.commands[3].args.at(-1), 'suite.third');
  h.children[2].complete(); h.children[3].complete(); await running;
  assert.equal(h.runs[0].failed.length, 1);
  assert.equal(h.runs[0].passed.length, 2);
  assert.equal(h.runs[0].errors.length, 0);
  h.dispose();
});

/** Overlapping editor requests share the same process budget and cancel queued work. */
test('concurrent runs share slots and cancellation does not launch queued tests', async () => {
  const h = harness(); h.options.holdRuns = true;
  h.client.response = listing('/workspace/main.sun'); h.start(); await flush();
  const first = h.controller.run({}, token()); await flush();
  const cancelled = event();
  const secondToken = { isCancellationRequested: false, onCancellationRequested: cancelled.listen };
  const second = h.controller.run({}, secondToken); await flush();
  assert.equal(h.children.length, 3);
  secondToken.isCancellationRequested = true; cancelled.fire(); await second;
  assert.equal(h.runs[1].skipped.length, 2);
  assert.equal(h.children.length, 3);
  h.children[1].complete(); h.children[2].complete(); await first;
  assert.equal(h.runs[0].passed.length, 2); h.dispose();
});

/** A queued selection cannot run against a binary rebuilt while other tests execute. */
test('queued tests recheck binary identity before execution', async () => {
  const h = harness(); h.options.holdRuns = true;
  h.client.response = listing('/workspace/main.sun');
  const payload = binaryListing();
  payload.tests.push({ ...payload.tests[0], id: 'suite.third', label: 'third' });
  h.options.listingText = JSON.stringify(payload);
  h.start(); await flush();
  const running = h.controller.run({}, token()); await flush();
  h.options.identity = 6n;
  h.children[1].complete(); h.children[2].complete(); await running;
  assert.equal(h.children.length, 3);
  assert.match(h.runs[0].errors[0], /binary changed/);
  h.dispose();
});

/** A native crash loses only its own test, rather than the entire suite. */
test('a crashed test process does not prevent its sibling from passing', async () => {
  const h = harness(); h.options.holdRuns = true;
  h.client.response = listing('/workspace/main.sun'); h.start(); await flush();
  const running = h.controller.run({}, token()); await flush();
  h.children[1].signalCode = 'SIGSEGV';
  h.children[1].emit('close', null, 'SIGSEGV');
  h.children[2].complete(); await running;
  assert.match(h.runs[0].errors[0], /SIGSEGV/);
  assert.equal(h.runs[0].passed.length, 1);
  assert.equal(h.runs[0].ended, true); h.dispose();
});

/** A parent and child selection still execute each unexcluded test only once. */
test('overlapping selections and exclusions retain exact test filters', async () => {
  const h = harness(); h.client.response = listing('/workspace/main.sun');
  h.start(); await flush();
  const root = h.controller.items.get('/workspace/main.sun');
  const leaves = [...[...root.children.values()][0].children.values()];
  await h.controller.run({ include: [root, leaves[0]], exclude: [leaves[1]] }, token());
  assert.deepEqual(h.commands.slice(1).map((c) => c.args), [
    ['--test-sequential', '--test-filter', 'suite.first'],
  ]);
  assert.equal(h.runs[0].passed.length, 1); h.dispose();
});

/** Queue feedback is synchronous even when freshness discovery has not returned. */
test('play immediately enqueues selected leaves before inventory completes', async () => {
  const h = harness(); h.client.response = listing('/workspace/main.sun');
  h.start(); await flush();
  const root = h.controller.items.get('/workspace/main.sun');
  const leaves = [...[...root.children.values()][0].children.values()];
  let release;
  const inventory = new Promise((resolve) => { release = resolve; });
  h.client.sendRequest = () => inventory;
  const running = h.controller.run({ include: [root], exclude: [leaves[1]] }, token());
  assert.deepEqual(h.runs[0].enqueued, [leaves[0].id]);
  assert.equal(h.runs[0].started.length, 0);
  assert.equal(h.runs[0].ended, false);
  release(h.client.response); await running;
  assert.deepEqual(h.runs[0].enqueued, [leaves[0].id]);
  assert.equal(h.runs[0].passed.length, 1);
  assert.equal(h.runs[0].ended, true); h.dispose();
});

/** Cancelling during inventory clears every queued indicator and ends the run. */
test('cancellation during inventory skips queued tests without starting processes', async () => {
  const h = harness(); h.client.response = listing('/workspace/main.sun');
  h.start(); await flush();
  let release;
  h.client.sendRequest = () => new Promise((resolve) => { release = resolve; });
  const cancelled = token();
  const running = h.controller.run({}, cancelled);
  assert.equal(h.runs[0].enqueued.length, 2);
  cancelled.isCancellationRequested = true;
  release(h.client.response); await running;
  assert.equal(h.runs[0].skipped.length, 2);
  assert.equal(h.runs[0].started.length, 0);
  assert.equal(h.runs[0].ended, true);
  assert.equal(h.commands.length, 1); h.dispose();
});

/** A previously empty suite can populate during refresh without a false removal error. */
test('a stale empty suite gains queued leaves when discovery finishes', async () => {
  const h = harness(); h.client.response = listing('/workspace/main.sun');
  h.client.response.suites[0].freshness = 'stale';
  h.options.listingText = JSON.stringify({ version: 1, tests: [] });
  h.start(); await flush();
  const root = h.controller.items.get('/workspace/main.sun');
  h.options.identity = 6n;
  h.client.response.suites[0].binary_identity = '1:2:3:4:6';
  h.options.listingText = JSON.stringify(binaryListing());
  await h.controller.run({ include: [root] }, token());
  assert.equal(h.runs[0].enqueued.length, 3);
  assert.deepEqual(h.runs[0].skipped, [root.id]);
  assert.equal(h.runs[0].passed.length, 2);
  assert.equal(h.runs[0].errors.length, 0); h.dispose();
});

/** A PASS line cannot hide a later crash during process shutdown. */
test('a process that crashes after PASS is reported as an error', async () => {
  const h = harness(); h.options.holdRuns = true;
  h.client.response = listing('/workspace/main.sun'); h.start(); await flush();
  const running = h.controller.run({}, token()); await flush();
  h.children[1].stdout.emit('data', Buffer.from('PASS suite.first\n'));
  assert.equal(h.runs[0].passed.length, 0);
  h.children[1].emit('close', null, 'SIGSEGV');
  h.children[2].complete(); await running;
  assert.match(h.runs[0].errors[0], /SIGSEGV/);
  assert.equal(h.runs[0].passed.length, 1); h.dispose();
});

/** Decoding process chunks must preserve non-ASCII failure messages. */
test('test output preserves Unicode split across chunks', async () => {
  const h = harness(); h.options.holdRuns = true;
  h.client.response = listing('/workspace/main.sun'); h.start(); await flush();
  const running = h.controller.run({}, token()); await flush();
  const bytes = Buffer.from('FAIL suite.first: expected 😀\r\n');
  const split = bytes.indexOf(Buffer.from('😀')) + 1;
  h.children[1].stdout.emit('data', bytes.subarray(0, split));
  h.children[1].complete(bytes.subarray(split), 1);
  h.children[2].complete(); await running;
  assert.match(h.runs[0].output.join(''), /expected 😀/);
  assert.ok(!h.runs[0].output.join('').includes('\r\r\n'));
  assert.equal(h.runs[0].failed.length, 1); h.dispose();
});

/** Suppressed production targets must still be observed under excluded build folders. */
test('artifact polling includes targets hidden by production metadata', async () => {
  const h = harness(); h.client.response = listing();
  h.client.response.watched_binaries = ['/workspace/build/program', '/workspace/build/program_test'];
  h.start(); await flush();
  assert.deepEqual([...h.polled.keys()], h.client.response.watched_binaries);
  h.polled.get('/workspace/build/program_test')();
  await debounce(); await flush();
  assert.equal(h.calls.filter((call) => call === 'sun/testSuites').length, 2);
  h.dispose();
});

/** Package selections run all nested suites while preserving entrypoint identities. */
test('package groups contain named entrypoints and run their selected binaries', async () => {
  const h = harness();
  h.client.response = listing('/workspace/one.sun', '/workspace/two.sun');
  for (const [index, suite] of h.client.response.suites.entries()) {
    suite.package_id = 'example/library';
    suite.entrypoint_name = ['unit', 'integration'][index];
    suite.build_location = index === 0 ? 'standalone' : 'workspace';
  }
  h.start(); await flush();
  assert.equal(h.controller.items.size, 1);
  const group = h.controller.items.get('package::example/library');
  assert.equal(group.label, 'example/library');
  assert.equal(group.description, 'Package');
  assert.equal(h.controller.label, 'Sun Test Explorer');
  assert.deepEqual([...group.children.values()].map(item => item.label), ['unit', 'integration']);
  assert.equal(group.children.get('/workspace/one.sun').description, 'Entrypoint');
  assert.equal(group.children.get('/workspace/two.sun').description, 'Entrypoint');
  await h.controller.run({ include: [group] }, token());
  assert.equal(h.runs[0].passed.length, 4);
  assert.equal(h.runs[0].errors.length, 0);
  assert.equal(group.children.get('/workspace/one.sun').description, 'Entrypoint');
  h.dispose();
});

/** Module selections span files, preserve nested scopes, and exclude sibling tests. */
test('modules group tests across files and run only selected descendants', async () => {
  const h = harness();
  h.client.response = listing('/workspace/main.sun');
  const fixture = binaryListing().tests[0];
  h.options.listingText = JSON.stringify({ version: 1, tests: [
    { ...fixture, id: 'acceptance.first', label: 'first' },
    { ...fixture, id: 'acceptance.second', label: 'second', file: '/workspace/other.sun' },
    { ...fixture, id: 'acceptance.http.first', label: 'first' },
    { ...fixture, id: 'unit.first', label: 'first' },
    { ...fixture, id: 'global_test', label: 'global_test' },
  ] });
  h.start(); await flush();
  const root = h.controller.items.get('/workspace/main.sun');
  assert.deepEqual([...root.children.values()].map(item => item.label), ['acceptance', 'unit', 'global_test']);
  const acceptance = root.children.get('/workspace/main.sun::module::acceptance');
  assert.equal(acceptance.description, 'Module');
  assert.deepEqual([...acceptance.children.values()].map(item => item.label), ['first', 'second', 'http']);
  const second = acceptance.children.get('/workspace/main.sun::acceptance.second');
  assert.equal(second.uri.fsPath, '/workspace/other.sun');
  const http = acceptance.children.get('/workspace/main.sun::module::acceptance.http');
  assert.equal(http.description, 'Module');
  assert.equal(http.children.get('/workspace/main.sun::acceptance.http.first').label, 'first');
  await h.controller.run({ include: [acceptance], exclude: [http] }, token());
  assert.deepEqual(h.commands.filter(command => command.args.includes('--test-filter'))
    .map(command => command.args.at(-1)), ['acceptance.first', 'acceptance.second']);
  assert.equal(h.runs[0].passed.length, 2);
  assert.equal(h.runs[0].errors.length, 0);
  await h.controller.run({ include: [http, root.children.get('/workspace/main.sun::global_test')] }, token());
  assert.deepEqual(h.commands.filter(command => command.args.includes('--test-filter'))
    .slice(-2).map(command => command.args.at(-1)), ['acceptance.http.first', 'global_test']);
  assert.equal(h.runs[1].passed.length, 2);
  assert.equal(h.runs[1].errors.length, 0);
  h.dispose();
});

/** Switching build locations refreshes the listing instead of running the old artifact. */
test('standalone selection replaces the workspace binary before execution', async () => {
  const h = harness(); h.client.response = listing('/workspace/main.sun');
  h.start(); await flush();
  const selected = h.controller.items.get('/workspace/main.sun');
  h.client.response.suites[0].test_binary = '/project/.sun/main_test';
  await h.controller.run({ include: [selected] }, token());
  assert.equal(h.runs[0].passed.length, 2);
  assert.equal(h.commands.at(-1).command, '/project/.sun/main_test');
  h.dispose();
});

/** Empty fresh suites disappear; stale empty listings still signal a rebuild. */
test('empty listings hide only fresh suites and remove empty packages', async () => {
  const h = harness();
  h.client.response = listing('/workspace/empty.sun', '/workspace/stale.sun');
  h.client.response.suites[0].package_id = 'empty/package';
  h.client.response.suites[1].freshness = 'stale';
  h.options.listingText = JSON.stringify({ version: 1, tests: [] });
  h.start(); await flush();
  assert.equal(h.controller.items.has('package::empty/package'), false);
  assert.match(h.controller.items.get('/workspace/stale.sun').description, /Needs rebuild/);
  h.dispose();
});
