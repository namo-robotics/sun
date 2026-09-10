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

/** Get configured manifest path variables */
function getPathVariables(): Record<string, string> {
  return vscode.workspace
    .getConfiguration('sun')
    .get<Record<string, string>>('path_variables', {});
}

/** The sun_configs setting resolved to paths, including files not created yet. */
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
    resolved.push(configPath);
  }
  return resolved;
}

/** Send sun-configs, entrypoints and path variables to LSP */
async function sendConfigurationToLSP(workspaceFolder: string | undefined): Promise<void> {
  if (!client) return;

  const entrypoints = getConfiguredEntrypoints(workspaceFolder);

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

/** Start language services and configured test discovery. */
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

  const sunConfigs = getSunConfigs(workspaceFolder);
  const entrypoints = getConfiguredEntrypoints(workspaceFolder);

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

  try {
    await client.start();
    activateTestExplorer(
      _context, client, env, workspaceFolder, command,
      () => sendConfigurationToLSP(workspaceFolder),
      () => getSunConfigs(workspaceFolder)
    );
  } catch (error) {
    const message = error instanceof Error ? error.message : String(error);
    void vscode.window.showErrorMessage(`Sun LSP failed to start: ${message}`);
  }
}

/** Stop the language server. */
export async function deactivate(): Promise<void> {
  if (!client) {
    return;
  }

  await client.stop();
  client = undefined;
}
