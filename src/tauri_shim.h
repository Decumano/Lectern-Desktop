// The frontend is a git submodule shared with the server (see the Lectern
// repos' `.gitmodules`), and its platform.js decides at load time whether it
// is running on the desktop by looking for `window.__TAURI__.core.invoke`.
// Nothing else in the frontend touches Tauri.
//
// Rather than fork the submodule — which would mean maintaining two copies of
// an 18,000-line file — this injects a shim that presents saucer's RPC under
// the name the frontend already looks for. It is the only Tauri-shaped thing
// left in the port, and it is 40 lines of JavaScript.
//
// The one impedance mismatch it resolves: Tauri's `invoke(name, {namedArgs})`
// passes arguments by name, while saucer's `window.saucer.call(name, [args])`
// passes them positionally. The table below is that mapping, and its rows must
// match the parameter order of the corresponding `expose()` call in main.cpp.
#pragma once

namespace lectern {

inline constexpr const char *kTauriShim = R"js(
(function () {
  // Argument order per command, matching the exposed C++ signatures.
  var SIGNATURES = {
    greet: ['name'],
    defaultFile: ['name'],
    save_file: ['name', 'content'],

    pick_work_folder: [],
    list_work_folder: ['root'],
    read_work_file: ['root', 'relPath'],
    write_work_file: ['root', 'relPath', 'content'],
    create_work_folder: ['root', 'relPath'],
    delete_work_entry: ['root', 'relPath', 'isDir'],
    move_work_entry: ['root', 'fromRelPath', 'toRelPath'],

    cloud_connect: ['serverUrl', 'email', 'password', 'root'],
    cloud_disconnect: [],
    cloud_status: [],
    cloud_sync_now: [],
    cloud_list_conflicts: [],
    cloud_resolve_conflict: ['relPath', 'choice'],
    cloud_list_fonts: [],
    cloud_font_data_url: ['fontId'],

    open_secondary_window: ['label', 'url', 'title', 'width', 'height', 'x', 'y']
  };

  function invoke(name, args) {
    var signature = SIGNATURES[name];
    if (!signature) {
      return Promise.reject(new Error("unknown command '" + name + "'"));
    }

    // Values pass through untouched; an argument the caller omitted becomes
    // JSON null on the wire, which the C++ side reads as the type's empty
    // value ("" or false) the way serde's defaults did.
    var params = signature.map(function (key) {
      return (args || {})[key];
    });

    // saucer rejects with the raw error string. main.js reports failures as
    // `e.message`, which on a bare string is undefined — so a real error
    // arrives as "Could not connect: undefined". Wrap it so the message the
    // C++ side produced actually reaches the user.
    return window.saucer.call(name, params).catch(function (reason) {
      throw reason instanceof Error ? reason : new Error(String(reason));
    });
  }

  window.__TAURI__ = window.__TAURI__ || {};
  window.__TAURI__.core = { invoke: invoke };

  // Tearing a tab out into its own window: the frontend does this through
  // Tauri's WebviewWindow constructor, so present the same shape. Only the
  // options the frontend actually passes are honoured; the returned object is
  // a placeholder, which is all it does with the result.
  //
  // Without this the frontend fell through to window.open(), which a WebView2
  // host ignores unless it handles NewWindowRequested — so dragging a tab out
  // did nothing at all, silently.
  function WebviewWindow(label, options) {
    options = options || {};
    this.label = String(label || '');

    invoke('open_secondary_window', {
      label: this.label,
      url: options.url || 'index.html',
      title: options.title || 'Lectern',
      width: options.width || 1100,
      height: options.height || 750,
      // -1 means "no drop point"; the backend then lets the OS place it.
      x: typeof options.x === 'number' ? Math.round(options.x) : -1,
      y: typeof options.y === 'number' ? Math.round(options.y) : -1
    }).catch(function (error) {
      console.error('could not open a window:', error && error.message || error);
    });
  }

  window.__TAURI__.webviewWindow = { WebviewWindow: WebviewWindow };
})();
)js";

}  // namespace lectern
