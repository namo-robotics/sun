import * as fs from 'node:fs';
import * as path from 'node:path';
import { spawnSync } from 'node:child_process';
import * as vscode from 'vscode';
import {
  LanguageClient,
  LanguageClientOptions,
  ServerOptions,
  TransportKind,
} from 'vscode-languageclient/node';

let client: LanguageClient | undefined;
let discoveredEntrypoints: Set<string> = new Set();
let fileWatcher: vscode.FileSystemWatcher | undefined;

function resolveServerCommand(configuredPath: string): string {
  if (path.isAbsolute(configuredPath) && fs.existsSync(configuredPath)) {
    return configuredPath;
  }

  const workspaceFolder = vscode.workspace.workspaceFolders?.[0]?.uri.fsPath;
  if (workspaceFolder) {
    const localBuildPath = path.join(workspaceFolder, configuredPath);
    if (fs.existsSync(localBuildPath)) {
      return localBuildPath;
    }
  }

  return configuredPath;
}

function commandExistsOnPath(command: string): boolean {
  const checkCommand = process.platform === 'win32' ? 'where' : 'which';
  const result = spawnSync(checkCommand, [command], { stdio: 'ignore' });
  return result.status === 0;
}

function canLaunchServer(command: string): boolean {
  if (path.isAbsolute(command)) {
    return fs.existsSync(command);
  }
  return commandExistsOnPath(command);
}

/** Check if a file contains a manifest block */
function fileContainsManifest(filePath: string): boolean {
  try {
    const content = fs.readFileSync(filePath, 'utf-8');
    // Look for 'manifest' followed by optional whitespace and '{'
    return /\bmanifest\s*\{/.test(content);
  } catch {
    return false;
  }
}

/** Scan workspace for .sun files containing manifest blocks */
async function discoverManifests(): Promise<Set<string>> {
  const manifests = new Set<string>();
  const files = await vscode.workspace.findFiles('**/*.sun', '**/node_modules/**');

  for (const file of files) {
    if (fileContainsManifest(file.fsPath)) {
      manifests.add(file.fsPath);
    }
  }

  return manifests;
}

/** Get manually configured entrypoints */
function getConfiguredEntrypoints(workspaceFolder: string | undefined): string[] {
  const configured = vscode.workspace
    .getConfiguration('sun')
    .get<Array<string | { path: string }>>('entrypoints', []);

  const resolved: string[] = [];
  for (const entry of configured) {
    const entryPath = typeof entry === 'string' ? entry : entry.path;
    let resolvedPath = entryPath;
    if (!path.isAbsolute(entryPath) && workspaceFolder) {
      resolvedPath = path.join(workspaceFolder, entryPath);
    }
    resolved.push(resolvedPath);
  }
  return resolved;
}

/** Merge manual config with discovered manifests (manual takes precedence) */
function getMergedEntrypoints(workspaceFolder: string | undefined): string[] {
  const manual = getConfiguredEntrypoints(workspaceFolder);

  // If manual config exists, use it exclusively
  if (manual.length > 0) {
    return manual;
  }

  // Otherwise use discovered manifests
  return Array.from(discoveredEntrypoints);
}

/** Send entrypoints update to LSP */
async function sendEntrypointsToLSP(workspaceFolder: string | undefined): Promise<void> {
  if (!client) return;

  const entrypoints = getMergedEntrypoints(workspaceFolder);

  await client.sendNotification('workspace/didChangeConfiguration', {
    settings: {
      sun: {
        entrypoints,
      },
    },
  });
}

export async function activate(_context: vscode.ExtensionContext): Promise<void> {
  const configuredPath = vscode.workspace
    .getConfiguration('sun')
    .get<string>('lsp.path', '/usr/bin/sun-lsp');

  const command = resolveServerCommand(configuredPath);

  if (!canLaunchServer(command)) {
    const selection = await vscode.window.showWarningMessage(
      `Sun LSP executable not found: ${command}. Set 'sun.lsp.path' to the full path of sun-lsp (for example /usr/bin/sun-lsp or /workspaces/sun/build/sun-lsp).`,
      'Open Settings'
    );

    if (selection === 'Open Settings') {
      await vscode.commands.executeCommand(
        'workbench.action.openSettings',
        'sun.lsp.path'
      );
    }
    return;
  }

  const env = { ...process.env };
  const workspaceFolder = vscode.workspace.workspaceFolders?.[0]?.uri.fsPath;

  // Build SUN_PATH: existing env + configured paths (or workspace folder if not configured)
  const sunPathParts: string[] = [];
  if (env.SUN_PATH) {
    sunPathParts.push(env.SUN_PATH);
  }

  const configuredPaths = vscode.workspace
    .getConfiguration('sun')
    .get<string[]>('sunPath', []);

  if (configuredPaths.length > 0) {
    for (const p of configuredPaths) {
      if (path.isAbsolute(p)) {
        sunPathParts.push(p);
      } else if (workspaceFolder) {
        sunPathParts.push(path.join(workspaceFolder, p));
      }
    }
  } else if (workspaceFolder) {
    sunPathParts.push(workspaceFolder);
  }

  if (sunPathParts.length > 0) {
    env.SUN_PATH = sunPathParts.join(':');
  }

  // Discover manifests in workspace
  discoveredEntrypoints = await discoverManifests();

  // Get merged entrypoints (manual config takes precedence)
  const entrypoints = getMergedEntrypoints(workspaceFolder);

  const serverOptions: ServerOptions = {
    run: { command, transport: TransportKind.stdio, options: { env } },
    debug: { command, transport: TransportKind.stdio, options: { env } },
  };

  const clientOptions: LanguageClientOptions = {
    documentSelector: [{ scheme: 'file', language: 'sun' }],
    initializationOptions: {
      entrypoints,
    },
  };

  client = new LanguageClient('sun-lsp', 'Sun Language Server', serverOptions, clientOptions);

  // Handle manual configuration changes
  const configChangeDisposable = vscode.workspace.onDidChangeConfiguration(async (e) => {
    if (e.affectsConfiguration('sun.entrypoints')) {
      await sendEntrypointsToLSP(workspaceFolder);
    }
  });

  // Watch for .sun file changes to update discovered manifests
  fileWatcher = vscode.workspace.createFileSystemWatcher('**/*.sun');

  const checkAndUpdateManifest = async (uri: vscode.Uri) => {
    const filePath = uri.fsPath;
    const hadManifest = discoveredEntrypoints.has(filePath);
    const hasManifest = fileContainsManifest(filePath);

    if (hasManifest && !hadManifest) {
      discoveredEntrypoints.add(filePath);
      await sendEntrypointsToLSP(workspaceFolder);
    } else if (!hasManifest && hadManifest) {
      discoveredEntrypoints.delete(filePath);
      await sendEntrypointsToLSP(workspaceFolder);
    }
  };

  fileWatcher.onDidCreate(checkAndUpdateManifest);
  fileWatcher.onDidChange(checkAndUpdateManifest);
  fileWatcher.onDidDelete(async (uri) => {
    if (discoveredEntrypoints.delete(uri.fsPath)) {
      await sendEntrypointsToLSP(workspaceFolder);
    }
  });

  try {
    await client.start();
    _context.subscriptions.push(configChangeDisposable);
    _context.subscriptions.push(fileWatcher);
  } catch (error) {
    configChangeDisposable.dispose();
    fileWatcher.dispose();
    const message = error instanceof Error ? error.message : String(error);
    void vscode.window.showErrorMessage(`Sun LSP failed to start: ${message}`);
  }
}

export async function deactivate(): Promise<void> {
  if (fileWatcher) {
    fileWatcher.dispose();
    fileWatcher = undefined;
  }

  if (!client) {
    return;
  }

  await client.stop();
  client = undefined;
}
