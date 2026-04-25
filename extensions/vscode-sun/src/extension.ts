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

function resolveServerCommand(configuredPath: string): string {
  if (path.isAbsolute(configuredPath) && fs.existsSync(configuredPath)) {
    return configuredPath;
  }

  const workspaceFolder = vscode.workspace.workspaceFolders?.[0]?.uri.fsPath;
  if (workspaceFolder) {
    const localBuildPath = path.join(workspaceFolder, 'build', configuredPath);
    if (fs.existsSync(localBuildPath)) {
      return localBuildPath;
    }

    const directBuildPath = path.join(workspaceFolder, 'build', 'sun-lsp');
    if (configuredPath === 'sun-lsp' && fs.existsSync(directBuildPath)) {
      return directBuildPath;
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

export async function activate(_context: vscode.ExtensionContext): Promise<void> {
  const configuredPath = vscode.workspace
    .getConfiguration('sun')
    .get<string>('lsp.path', 'sun-lsp');

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

  const serverOptions: ServerOptions = {
    run: { command, transport: TransportKind.stdio },
    debug: { command, transport: TransportKind.stdio },
  };

  const clientOptions: LanguageClientOptions = {
    documentSelector: [{ scheme: 'file', language: 'sun' }],
  };

  client = new LanguageClient('sun-lsp', 'Sun Language Server', serverOptions, clientOptions);

  try {
    await client.start();
  } catch (error) {
    const message = error instanceof Error ? error.message : String(error);
    void vscode.window.showErrorMessage(`Sun LSP failed to start: ${message}`);
  }
}

export async function deactivate(): Promise<void> {
  if (!client) {
    return;
  }

  await client.stop();
  client = undefined;
}
