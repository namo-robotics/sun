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
import { activateTestExplorer } from './testExplorer';

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

/** Get configured manifest path variables */
function getPathVariables(): Record<string, string> {
  return vscode.workspace
    .getConfiguration('sun')
    .get<Record<string, string>>('path_variables', {});
}

/** The sun_configs setting resolved to absolute paths of files that exist. */
export function getSunConfigs(workspaceFolder: string | undefined): string[] {
  const configured = vscode.workspace
    .getConfiguration('sun')
    .get<string[]>('sun_configs', ['sun-config.json']);
  const resolved: string[] = [];
  for (const entry of configured) {
    const configPath =
      path.isAbsolute(entry) || !workspaceFolder
        ? entry
        : path.join(workspaceFolder, entry);
    if (fs.existsSync(configPath)) {
      resolved.push(configPath);
    }
  }
  return resolved;
}

/** True when any resolved config file declares a non-empty entrypoints
 *  list — then the configs describe the project and the manifest scan is
 *  unnecessary. */
function configsDeclareEntrypoints(configPaths: string[]): boolean {
  for (const configPath of configPaths) {
    try {
      const parsed = JSON.parse(fs.readFileSync(configPath, 'utf-8'));
      if (Array.isArray(parsed.entrypoints) && parsed.entrypoints.length > 0) {
        return true;
      }
    } catch {
      // A malformed config is the server's to report; keep scanning.
    }
  }
  return false;
}

/** Send sun-configs, entrypoints and path variables to LSP */
async function sendConfigurationToLSP(workspaceFolder: string | undefined): Promise<void> {
  if (!client) return;

  const entrypoints = getMergedEntrypoints(workspaceFolder);

  await client.sendNotification('workspace/didChangeConfiguration', {
    settings: {
      sun: {
        sun_configs: getSunConfigs(workspaceFolder),
        entrypoints,
        pathVariables: getPathVariables(),
      },
    },
  });
}

export async function activate(_context: vscode.ExtensionContext): Promise<void> {
  const configuredPath = vscode.workspace
    .getConfiguration('sun')
    .get<string>('lsp_path', '/usr/bin/sun-lsp');

  const command = resolveServerCommand(configuredPath);

  if (!canLaunchServer(command)) {
    const selection = await vscode.window.showWarningMessage(
      `Sun LSP executable not found: ${command}. Set 'sun.lsp_path' to the full path of sun-lsp (for example /usr/bin/sun-lsp or /workspaces/sun/build/sun-lsp).`,
      'Open Settings'
    );

    if (selection === 'Open Settings') {
      await vscode.commands.executeCommand(
        'workbench.action.openSettings',
        'sun.lsp_path'
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
    .get<string[]>('sun_path', []);

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

  // Discover entrypoints. When a sun-config declares them, the configs
  // describe the project and the manifest scan is skipped; otherwise every
  // .sun file with a manifest block is an entrypoint candidate.
  const sunConfigs = getSunConfigs(workspaceFolder);
  discoveredEntrypoints = configsDeclareEntrypoints(sunConfigs)
    ? new Set()
    : await discoverManifests();

  // Get merged entrypoints (manual config takes precedence)
  const entrypoints = getMergedEntrypoints(workspaceFolder);

  const serverOptions: ServerOptions = {
    run: { command, transport: TransportKind.stdio, options: { env } },
    debug: { command, transport: TransportKind.stdio, options: { env } },
  };

  const clientOptions: LanguageClientOptions = {
    documentSelector: [{ scheme: 'file', language: 'sun' }],
    initializationOptions: {
      sun_configs: sunConfigs,
      entrypoints,
      pathVariables: getPathVariables(),
    },
  };

  client = new LanguageClient('sun-lsp', 'Sun Language Server', serverOptions, clientOptions);

  // Handle manual configuration changes
  const configChangeDisposable = vscode.workspace.onDidChangeConfiguration(async (e) => {
    if (
      e.affectsConfiguration('sun.entrypoints') ||
      e.affectsConfiguration('sun.sun_configs') ||
      e.affectsConfiguration('sun.path_variables')
    ) {
      await sendConfigurationToLSP(workspaceFolder);
    }
  });

  // An edited sun-config.json changes entrypoints and path variables;
  // resending the settings makes the server re-read the files.
  const configFileWatcher =
    vscode.workspace.createFileSystemWatcher('**/sun-config.json');
  configFileWatcher.onDidCreate(() => sendConfigurationToLSP(workspaceFolder));
  configFileWatcher.onDidChange(() => sendConfigurationToLSP(workspaceFolder));
  configFileWatcher.onDidDelete(() => sendConfigurationToLSP(workspaceFolder));
  _context.subscriptions.push(configFileWatcher);

  // Watch for .sun file changes to update discovered manifests
  fileWatcher = vscode.workspace.createFileSystemWatcher('**/*.sun');

  const checkAndUpdateManifest = async (uri: vscode.Uri) => {
    const filePath = uri.fsPath;
    const hadManifest = discoveredEntrypoints.has(filePath);
    const hasManifest = fileContainsManifest(filePath);

    if (hasManifest && !hadManifest) {
      discoveredEntrypoints.add(filePath);
      await sendConfigurationToLSP(workspaceFolder);
    } else if (!hasManifest && hadManifest) {
      discoveredEntrypoints.delete(filePath);
      await sendConfigurationToLSP(workspaceFolder);
    }
  };

  fileWatcher.onDidCreate(checkAndUpdateManifest);
  fileWatcher.onDidChange(checkAndUpdateManifest);
  fileWatcher.onDidDelete(async (uri) => {
    if (discoveredEntrypoints.delete(uri.fsPath)) {
      await sendConfigurationToLSP(workspaceFolder);
    }
  });

  try {
    await client.start();
    _context.subscriptions.push(configChangeDisposable);
    _context.subscriptions.push(fileWatcher);
    activateTestExplorer(_context, client, env, workspaceFolder, command);
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
