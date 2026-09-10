const assert = require('node:assert/strict');
const { EventEmitter } = require('node:events');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const { test } = require('node:test');

function event() {
  const listeners = [];
  return {
    listen(fn) {
      listeners.push(fn);
      return { dispose() { listeners.splice(listeners.indexOf(fn), 1); } };
    },
    fire(value) { for (const fn of [...listeners]) fn(value); },
  };
}

class Items extends Map {
  add(item) { this.set(item.id, item); }
  replace(items) { this.clear(); for (const item of items) this.add(item); }
}

function load(name, mocks) {
  const filename = path.join(__dirname, '../out', name + '.js');
  const exports = {};
  vm.runInNewContext(fs.readFileSync(filename, 'utf8'), {
    exports, process, Buffer, setTimeout, clearTimeout,
    require: (id) => id in mocks ? mocks[id] : require(id),
  }, { filename });
  return exports;
}

function harness(fileSystem = fs) {
  const saved = event();
  const settingsChanged = event();
  const watchers = [];
  const settings = {};
  const calls = [];
  const commands = [];
  const controller = {
    items: new Items(),
    createTestItem: (id, label, uri) => ({ id, label, uri, children: new Items() }),
    createRunProfile(_name, _kind, run) { this.run = run; },
    createTestRun: () => ({ started() {}, passed() {}, errored() {}, appendOutput() {}, end() {} }),
    dispose() {},
  };
  const uri = (file) => ({ fsPath: file, scheme: 'file', toString: () => 'file://' + file });
  const vscode = {
    Uri: { file: uri, parse: (value) => uri(value.replace('file://', '')) },
    Range: class {},
    RelativePattern: class { constructor(base, pattern) { this.baseUri = base; this.pattern = pattern; } },
    TestRunProfileKind: { Run: 1 },
    TestMessage: class {},
    tests: { createTestController: () => controller },
    workspace: {
      workspaceFolders: [{ uri: uri('/workspace') }],
      getConfiguration: () => ({ get: (key, fallback) => settings[key] ?? fallback }),
      onDidSaveTextDocument: saved.listen,
      onDidChangeConfiguration: settingsChanged.listen,
      createFileSystemWatcher(pattern) {
        const create = event(), change = event(), remove = event();
        const watcher = {
          pattern, create, change, remove, disposed: false,
          onDidCreate: create.listen, onDidChange: change.listen, onDidDelete: remove.listen,
          dispose() { this.disposed = true; },
        };
        watchers.push(watcher);
        return watcher;
      },
    },
    window: { createOutputChannel: () => ({ appendLine() {}, dispose() {} }) },
  };
  const client = {
    async sendRequest(method) { calls.push(method); return client.response; },
    response: { entrypoints: [] },
  };
  const mocks = {
    vscode,
    'vscode-languageclient/node': {},
    'node:fs': fileSystem,
    'node:child_process': {
      spawn(command, args) {
        commands.push({ command, args: [...args] });
        const child = new EventEmitter();
        child.stdout = new EventEmitter(); child.stderr = new EventEmitter();
        setImmediate(() => { child.stdout.emit('data', Buffer.from('PASS suite.first\n')); child.emit('close', 0); });
        return child;
      },
    },
  };
  const context = { subscriptions: [] };
  const activate = load('testExplorer', mocks).activateTestExplorer;
  const start = (sync = async () => { calls.push('settings'); }) => activate(
    context, client, {}, '/workspace', '/missing/sun-lsp', sync,
    () => settings.sun_configs ?? ['/outside/custom.json']
  );
  return { saved, settingsChanged, watchers, settings, calls, commands, controller, uri,
    client, mocks, start, dispose: () => context.subscriptions.forEach((d) => d.dispose()) };
}

const flush = () => new Promise((resolve) => setImmediate(resolve));
const listing = (...entries) => ({ entrypoints: entries.map((entrypoint) => ({
  entrypoint, test_binary: null, sources: [entrypoint, '/workspace/tests.sun'],
  files: [{ uri: 'file:///workspace/tests.sun', tests: ['first', 'second'].map((name) => ({
    id: 'suite.' + name, label: name, range: { start: { line: 0, character: 0 }, end: { line: 0, character: 1 } },
  })) }],
})) });

test('startup and saves use workspace discovery; shared files retain both suites', async () => {
  const h = harness();
  h.client.response = listing('/workspace/one.sun', '/workspace/two.sun');
  h.start(); await flush();
  assert.deepEqual(h.calls, ['settings', 'sun/workspaceTests']);
  assert.equal(h.controller.items.size, 2);
  h.saved.fire({ languageId: 'sun', uri: h.uri('/workspace/tests.sun') });
  await flush();
  assert.equal(h.controller.items.size, 2);
  assert.ok(h.calls.every((call) => call !== 'sun/tests'));
  const root = h.controller.items.get('/workspace/one.sun');
  const leaf = [...root.children.values()][0].children.values().next().value;
  await h.controller.run({ include: [leaf] }, {
    isCancellationRequested: false, onCancellationRequested: () => ({ dispose() {} }),
  });
  assert.deepEqual(h.commands, [{ command: 'sun', args: ['test', '/workspace/one.sun', '--test-filter', 'suite.first'] }]);
  h.dispose();
});

test('no configured suites produce an empty tree and register no open-document discovery', async () => {
  const h = harness();
  h.start(); await flush();
  assert.equal(h.controller.items.size, 0);
  // The mock deliberately provides no document-open API or workspace scanning API.
  assert.deepEqual(h.calls, ['settings', 'sun/workspaceTests']);
  h.dispose();
});

test('custom config creation, editing, deletion and setting changes refresh in order', async () => {
  const h = harness();
  h.start(); await flush();
  const watcher = h.watchers[0];
  assert.equal(watcher.pattern.baseUri.fsPath, '/outside');
  for (const action of ['create', 'change', 'remove']) {
    h.client.response = action === 'remove' ? listing() : listing('/workspace/main.sun');
    watcher[action].fire(h.uri('/outside/custom.json'));
    await flush();
    assert.equal(h.controller.items.size, action === 'remove' ? 0 : 1);
  }
  const count = h.calls.length;
  watcher.change.fire(h.uri('/outside/unrelated.json')); await flush();
  assert.equal(h.calls.length, count);
  h.settings.sun_configs = ['/different/project.json'];
  h.settingsChanged.fire({ affectsConfiguration: (name) => name === 'sun.sun_configs' });
  await flush();
  assert.equal(watcher.disposed, true);
  assert.equal(h.watchers.at(-1).pattern.baseUri.fsPath, '/different');
  assert.deepEqual(h.calls, Array.from({ length: 5 }, () => ['settings', 'sun/workspaceTests']).flat());
  h.dispose();
});

test('overlapping refreshes are coalesced and never publish stale results', async () => {
  const h = harness();
  let complete;
  let requestCount = 0;
  h.client.sendRequest = async () => {
    requestCount++;
    if (requestCount === 1) return new Promise((resolve) => { complete = resolve; });
    assert.equal(h.controller.items.size, 0);
    return listing('/workspace/current.sun');
  };
  h.start(); await flush();
  const pending = h.controller.refreshHandler();
  h.controller.refreshHandler();
  assert.equal(requestCount, 1);
  complete(listing('/workspace/stale.sun'));
  await pending;
  assert.equal(requestCount, 2);
  assert.deepEqual([...h.controller.items.keys()], ['/workspace/current.sun']);
  h.dispose();
});

test('configuration preserves missing paths and never scans for manifests', async () => {
  const h = harness();
  let options;
  let explorerStarted = false;
  h.settings.lsp_path = process.execPath;
  h.settings.sun_configs = ['not-created.json', '/outside/project.json'];
  h.settings.entrypoints = [{ path: 'main.sun' }];
  h.mocks['vscode-languageclient/node'] = {
    TransportKind: { stdio: 0 },
    LanguageClient: class {
      constructor(_id, _name, _server, clientOptions) { options = clientOptions; }
      async start() {}
      async stop() {}
    },
  };
  h.mocks['./testExplorer'] = { activateTestExplorer() { explorerStarted = true; } };
  const extension = load('extension', h.mocks);
  await extension.activate({ subscriptions: [] });
  assert.deepEqual(Array.from(options.initializationOptions.sun_configs), ['/workspace/not-created.json', '/outside/project.json']);
  assert.deepEqual(Array.from(options.initializationOptions.entrypoints), ['/workspace/main.sun']);
  assert.equal(explorerStarted, true);
  await extension.deactivate();
});


test('fresh configured test binaries still receive selected test filters', async () => {
  const h = harness({
    existsSync: () => false,
    statSync: (file) => ({ mtimeMs: file === '/workspace/suite_test' ? 20 : 10 }),
  });
  h.client.response = listing('/workspace/main.sun');
  h.client.response.entrypoints[0].test_binary = '/workspace/suite_test';
  h.start(); await flush();
  const root = h.controller.items.get('/workspace/main.sun');
  const leaf = [...root.children.values()][0].children.values().next().value;
  await h.controller.run({ include: [leaf] }, {
    isCancellationRequested: false, onCancellationRequested: () => ({ dispose() {} }),
  });
  assert.deepEqual(h.commands, [{ command: '/workspace/suite_test', args: ['--test-filter', 'suite.first'] }]);
  h.dispose();
});

test('a failed discovery can be refreshed again and disposal drops pending results', async () => {
  const h = harness();
  h.client.sendRequest = async () => { throw new Error('server unavailable'); };
  h.start(); await flush();
  h.client.sendRequest = async () => listing('/workspace/main.sun');
  await h.controller.refreshHandler();
  assert.equal(h.controller.items.size, 1);
  let complete;
  h.client.sendRequest = async () => new Promise((resolve) => { complete = resolve; });
  const pending = h.controller.refreshHandler();
  await flush();
  h.dispose();
  complete(listing('/workspace/stale.sun'));
  await pending;
  assert.deepEqual([...h.controller.items.keys()], ['/workspace/main.sun']);
});
