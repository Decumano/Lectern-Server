// Platform adapter: isolates every native file-system call behind one small
// interface, so main.js never talks to window.__TAURI__ (or the web API)
// directly. On Tauri, calls go through window.__TAURI__.core.invoke. In this
// web build, when Tauri isn't present, the same six work-folder methods are
// backed by fetch() calls to this server's /api/workspace/* endpoints instead
// of rejecting as "unavailable" — the server derives the workspace root from
// the logged-in session, so `root` is never actually sent to it.

var Platform = (function () {
  var tauri = window.__TAURI__ && window.__TAURI__.core;

  // Sentinel returned by pickWorkFolder() once authenticated: there's no
  // folder to pick on the web, just an implicit per-user workspace.
  var CLOUD_ROOT = 'cloud';

  function api(path, options) {
    options = options || {};
    options.credentials = 'include';
    return fetch('/api' + path, options).then(function (res) {
      if (!res.ok) {
        return res.json().catch(function () { return {}; }).then(function (body) {
          throw new Error(body.error || ('Request failed: ' + res.status));
        });
      }
      return res;
    });
  }

  function apiJson(path, options) {
    return api(path, options).then(function (res) { return res.json(); });
  }

  function pickWorkFolder() {
    if (tauri) return tauri.invoke('pick_work_folder');
    return currentUser().then(function (user) {
      return user ? CLOUD_ROOT : null;
    });
  }

  function listWorkFolder(root) {
    if (tauri) return tauri.invoke('list_work_folder', { root: root });
    return apiJson('/workspace');
  }

  function readWorkFile(root, relPath) {
    if (tauri) return tauri.invoke('read_work_file', { root: root, relPath: relPath });
    return api('/workspace/file?path=' + encodeURIComponent(relPath)).then(function (res) { return res.text(); });
  }

  function writeWorkFile(root, relPath, content) {
    if (tauri) return tauri.invoke('write_work_file', { root: root, relPath: relPath, content: content });
    return api('/workspace/file?path=' + encodeURIComponent(relPath), {
      method: 'PUT',
      headers: { 'Content-Type': 'text/plain;charset=utf-8' },
      body: content
    }).then(function () { return undefined; });
  }

  function createWorkFolder(root, relPath) {
    if (tauri) return tauri.invoke('create_work_folder', { root: root, relPath: relPath });
    return api('/workspace/folder', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ relPath: relPath })
    }).then(function () { return undefined; });
  }

  function deleteWorkEntry(root, relPath, isDir) {
    if (tauri) return tauri.invoke('delete_work_entry', { root: root, relPath: relPath, isDir: isDir });
    return api('/workspace/entry?path=' + encodeURIComponent(relPath) + '&isDir=' + !!isDir, {
      method: 'DELETE'
    }).then(function () { return undefined; });
  }

  function moveWorkEntry(root, fromRelPath, toRelPath) {
    if (tauri) return tauri.invoke('move_work_entry', { root: root, fromRelPath: fromRelPath, toRelPath: toRelPath });
    return api('/workspace/move', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ from: fromRelPath, to: toRelPath })
    }).then(function () { return undefined; });
  }

  // ── Auth (web only; no-ops don't apply to Tauri, which has no accounts) ──

  function currentUser() {
    if (tauri) return Promise.resolve(null);
    return fetch('/api/auth/me', { credentials: 'include' }).then(function (res) {
      return res.ok ? res.json() : null;
    });
  }

  function register(email, password) {
    return apiJson('/auth/register', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ email: email, password: password })
    });
  }

  function login(email, password) {
    return apiJson('/auth/login', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ email: email, password: password })
    });
  }

  function logout() {
    return api('/auth/logout', { method: 'POST' }).then(function () { return undefined; });
  }

  var DEFAULT_FILE_CONTENT = [
    "# Welcome to Lore Keep\n\n",
    "Lore Keep is a lightweight office suite that stores everything in **Markdown**.\n\n",
    "## Features\n\n",
    "**Documents** rich markdown editing with live preview\n",
    "**Spreadsheets** formula-capable grid stored as cell=value pairs\n",
    "## Markdown Quick Reference\n",
    "| Element | Syntax |\n",
    "|---------|--------|\n",
    "| Bold | **text** |\n",
    "| Italic | *text* |\n",
    "| Heading | # H1 ## H2 |\n",
    "| List | - item |\n",
    "| Blockquote | > text |\n",
    "| Code | ``` code ``` |\n",
    "| Link | [text](url) |\n\n",
    "## Getting Started\n\n",
    "1. Click **New** in the sidebar to create a file\n",
    "2. Write in Markdown, use the toolbar for formatting\n",
    "3. Toggle the **split** view button to preview alongside your writing\n",
    "4. Click the **download** button to export your file\n\n",
    "> All files are saved automatically to your browser's local storage.\n\n",
    "Happy writing!"
  ].join('');

  // Returns canned welcome content for a brand-new install. On Tauri this
  // matches the Rust-side `defaultFile` command; elsewhere it's generated
  // here so first-run works without a native backend.
  function defaultFile(name) {
    if (tauri) return tauri.invoke('defaultFile', { name: name });
    return Promise.resolve({
      name: 'Welcome to Lore Keep ' + name,
      docType: 'doc',
      content: DEFAULT_FILE_CONTENT
    });
  }

  // Triggers a standard browser download of a Blob under the given name.
  function downloadBlob(name, blob) {
    var url = URL.createObjectURL(blob);
    var a = document.createElement('a');
    a.href = url;
    a.download = name;
    document.body.appendChild(a);
    a.click();
    document.body.removeChild(a);
    URL.revokeObjectURL(url);
  }

  // Saves a one-off exported file. On Tauri this opens a native save dialog;
  // elsewhere it triggers a standard browser download of the same content.
  function saveFile(name, content) {
    if (tauri) return tauri.invoke('save_file', { name: name, content: content });
    try {
      downloadBlob(name, new Blob([content], { type: 'text/plain;charset=utf-8' }));
      return Promise.resolve(true);
    } catch (e) {
      return Promise.reject(e);
    }
  }

  // Renders `html` to a PDF server-side (headless Chromium, headers/footers
  // disabled - see src/export.rs) and downloads the result. Not available on
  // Tauri, which renders/prints PDFs locally via the print-preview flow instead.
  function exportPdf(name, html) {
    if (tauri) return Promise.reject(new Error('exportPdf is web-only'));
    return api('/export/pdf', {
      method: 'POST',
      headers: { 'Content-Type': 'text/html;charset=utf-8' },
      body: html
    }).then(function (res) {
      return res.blob();
    }).then(function (blob) {
      downloadBlob(name, blob);
      return true;
    });
  }

  return {
    isNative: !!tauri,
    pickWorkFolder: pickWorkFolder,
    listWorkFolder: listWorkFolder,
    readWorkFile: readWorkFile,
    writeWorkFile: writeWorkFile,
    createWorkFolder: createWorkFolder,
    deleteWorkEntry: deleteWorkEntry,
    moveWorkEntry: moveWorkEntry,
    defaultFile: defaultFile,
    saveFile: saveFile,
    exportPdf: exportPdf,
    currentUser: currentUser,
    register: register,
    login: login,
    logout: logout
  };
})();
