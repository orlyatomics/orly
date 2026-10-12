import * as path from 'path';
import * as fs from 'fs';
import * as vscode from 'vscode';
import {
  LanguageClient,
  LanguageClientOptions,
  ServerOptions,
  TransportKind
} from 'vscode-languageclient/node';

let client: LanguageClient | undefined;

function findBinary(configuredPath: string, binaryName: string): string | undefined {
  if (configuredPath && configuredPath.trim().length > 0) {
    return configuredPath.trim();
  }

  const workspaceFolders = vscode.workspace.workspaceFolders;
  if (workspaceFolders) {
    for (const folder of workspaceFolders) {
      const candidates = [
        path.join(folder.uri.fsPath, '..', 'out_orly', 'debug', 'orly', binaryName),
        path.join(folder.uri.fsPath, '..', 'out_orly', 'release', 'orly', binaryName),
        path.join(folder.uri.fsPath, 'out_orly', 'debug', 'orly', binaryName),
        path.join(folder.uri.fsPath, 'out_orly', 'release', 'orly', binaryName),
        path.join(folder.uri.fsPath, 'tools', binaryName)
      ];
      for (const cand of candidates) {
        if (fs.existsSync(cand)) {
          return cand;
        }
      }
    }
  }

  return binaryName;
}

export function activate(context: vscode.ExtensionContext) {
  const config = vscode.workspace.getConfiguration('orly');
  const lspEnabled = config.get<boolean>('lsp.enabled', true);

  if (lspEnabled) {
    startLsp(context);
  }

  context.subscriptions.push(
    vscode.commands.registerCommand('orly.restartLsp', async () => {
      if (client) {
        await client.stop();
        client = undefined;
      }
      startLsp(context);
      vscode.window.showInformationMessage('Orly Language Server restarted.');
    })
  );

  context.subscriptions.push(
    vscode.commands.registerCommand('orly.runLocalOrlyi', () => {
      const editor = vscode.window.activeTextEditor;
      if (!editor) {
        vscode.window.showWarningMessage('No active Orlyscript file to run.');
        return;
      }

      const filePath = editor.document.uri.fsPath;
      const workspaceFolder = vscode.workspace.getWorkspaceFolder(editor.document.uri);
      const cwd = workspaceFolder ? workspaceFolder.uri.fsPath : path.dirname(filePath);

      let terminal = vscode.window.terminals.find(t => t.name === 'Orly Local Runner');
      if (!terminal) {
        terminal = vscode.window.createTerminal({ name: 'Orly Local Runner', cwd });
      }

      terminal.show();
      // If repo dev script is present, run via dev lang or orlyc / orlyi
      const devScript = path.join(cwd, 'dev');
      if (fs.existsSync(devScript)) {
        terminal.sendText(`./dev lang ${path.relative(cwd, filePath)}`);
      } else {
        const orlyiBin = findBinary(config.get<string>('orlyi.path', ''), 'orlyi') || 'orlyi';
        terminal.sendText(`${orlyiBin} -s "${filePath}"`);
      }
    })
  );
}

function startLsp(context: vscode.ExtensionContext) {
  const config = vscode.workspace.getConfiguration('orly');
  const configuredLspPath = config.get<string>('lsp.path', '');
  const serverPath = findBinary(configuredLspPath, 'orly_lsp') || 'orly-lsp';

  const serverOptions: ServerOptions = {
    command: serverPath,
    args: [],
    transport: TransportKind.stdio
  };

  const clientOptions: LanguageClientOptions = {
    documentSelector: [{ scheme: 'file', language: 'orly' }],
    synchronize: {
      fileEvents: vscode.workspace.createFileSystemWatcher('**/*.orly')
    }
  };

  client = new LanguageClient(
    'orlyLanguageServer',
    'Orly Language Server',
    serverOptions,
    clientOptions
  );

  client.start().catch((err: unknown) => {
    vscode.window.showErrorMessage(`Failed to start Orly Language Server: ${String(err)}`);
  });

  context.subscriptions.push({
    dispose: () => {
      if (client) {
        return client.stop();
      }
    }
  });
}

export function deactivate(): Thenable<void> | undefined {
  if (!client) {
    return undefined;
  }
  return client.stop();
}
