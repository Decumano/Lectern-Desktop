# Lectern Desktop (C++)

A C++ port of [Lectern-Desktop](../Lectern-Desktop). Same app, same frontend,
same cloud-sync behaviour; the Tauri/Rust backend is replaced by
[saucer](https://github.com/saucer/saucer) — a C++ webview library that drives
WebView2 on Windows, WKWebView on macOS and WebKitGTK on Linux, which is the
same "use the OS webview" model Tauri uses.

The frontend is the `officesuite-frontend` submodule, shared with
[Lectern-Server-Cpp](../Lectern-Server-Cpp)'s `web/`. **No file in it was
modified for this port** — see the shim note below.

## Building

Needs a C++23 compiler (saucer's headers use `std::move_only_function` and
deducing `this`), CMake ≥ 3.25 and vcpkg.

The frontend lives in the `frontend/` submodule, which a fresh clone has to
check out:

```bash
git clone --recurse-submodules <this-repo>
```

In a clone that already exists:

```bash
git submodule update --init --recursive
```

That works because the submodule is registered in the index — a `.gitmodules`
file alone is not enough, and `git submodule update` will silently do nothing
without the matching gitlink. If `git submodule status` prints nothing, the
submodule was never added; fix it with:

```bash
git submodule add https://github.com/Decumano/officesuite-frontend.git frontend
```

```bash
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_TOOLCHAIN_FILE=E:/vcpkg/scripts/buildsystems/vcpkg.cmake
```

```bash
cmake --build build --config Release
```

The frontend directory is baked in at configure time via `LECTERN_FRONTEND_DIR`
(default: `./frontend`), and `LECTERN_FRONTEND_DIR` in the environment overrides
it at runtime — useful for pointing a build tree at an existing checkout:

```bash
LECTERN_FRONTEND_DIR=../Lectern-Desktop/src ./build/Release/lectern
```

## Tests

```bash
./build/Release/lectern-desktop-tests
```

## What changed, and why

**The frontend is served over `app://`, not `file://`.** Tauri used a custom
protocol; so does this. It is not cosmetic — the editor imports ES modules
(pdf.js), and Chromium blocks module imports across `file://` origins, so a
file-loaded page would silently lose PDF import and annotation. `serve_frontend`
in `main.cpp` handles the scheme, with the same path validation the work-folder
commands use so a page cannot read outside its own bundle.

**A 40-line shim presents saucer's RPC as `window.__TAURI__.core.invoke`.**
`platform.js` decides at load time whether it is on the desktop by looking for
that global, and nothing else in the frontend touches Tauri. Forking the
submodule to change one detection would mean maintaining two copies of an
18,000-line file, so `src/tauri_shim.h` bridges instead. It also maps Tauri's
named arguments onto saucer's positional ones; that table has to stay in step
with the `expose()` calls in `main.cpp`.

**The three-way merge is a fresh implementation.** The Rust build got its diff
from the `similar` crate. `src/diff.cpp` is a direct Myers implementation
producing the same hunk shape — a half-open range in the base text plus its
replacement lines — so `three_way_merge` is a line-by-line port of the original.
All seven of the Rust build's merge tests pass unchanged, including the two
subtle ones about identical insertions not being duplicated. The diff caps its
edit-distance search at 4000; past that it returns one whole-file hunk, which
the merge treats as an overlapping edit and reports as a conflict for the user
to resolve.

**File dialogs are per-platform.** saucer 6.0.1 as packaged by vcpkg ships no
desktop/dialog module (Tauri had `tauri-plugin-dialog`), so `src/dialogs.cpp`
uses `IFileDialog` on Windows, `GtkFileDialog` on Linux and osascript on macOS.
GTK is not an added dependency there — WebKitGTK already links it into the
process — and going through it rather than shelling out to zenity is what makes
the Flatpak build work at all; see Packaging below. zenity and kdialog remain a
fallback for builds where GTK 4 is not found.

**The JSON bridge is nlohmann, not glaze.** saucer 6.0.1's glaze serializer
spells its concepts `glz::write_supported<Format, T>` while glaze ≥ 5.1 declares
them `<T, Format>`, so every `expose()` fails to compile with "T should be
serializable" regardless of the type involved. That is a version skew between
two vcpkg ports. Rather than pin an old glaze, `src/bridge.h` plugs a
nlohmann-backed serializer into saucer's documented `generic::serializer`
extension point. The wire format is saucer's, unchanged.

**Writes are atomic.** `util::write_file_atomic` writes to a unique sibling and
renames, so a crash or a concurrent sync pass can't leave a half-written
document. The Rust build wrote in place.

## Verified

- Builds clean and links against saucer/WebView2
- 78 unit assertions pass: all seven original merge cases, the diff's hunk
  shapes, path validation, and the updater's signature checks
- The app launches, serves the frontend over `app://`, and renders the full
  Lectern UI — sidebar, tab bar, toolbar, editor
- The IPC bridge round-trips structured JSON in both directions: the welcome
  document on screen is the `defaultFile` command's response

Not exercised: the work-folder and cloud-sync commands past the folder-picker
dialog, which needs a click. The code paths behind them are ports of the Rust
originals and the merge engine under cloud sync is covered by the tests, but
they have not been run against a live server from the built app.

## Packaging

saucer is a webview library, not an application framework, so none of this comes
for free the way it did with Tauri. It is all built out here.

### Installers

`cpack` produces the same artifact set the Tauri build published:

| Platform | Generator | Artifact |
|---|---|---|
| Windows | NSIS | `Lectern-<v>-windows-x64-setup.exe` |
| Windows | WiX | `Lectern-<v>-windows-x64.msi` |
| Windows | ZIP | `Lectern-<v>-windows-x64.zip` (portable) |
| Linux | DEB / RPM / TGZ | `lectern_<v>_amd64.deb`, … |
| Linux | `scripts/build-appimage.sh` | `Lectern-<v>-linux-x86_64.AppImage` |
| macOS | DragNDrop | `Lectern-<v>-macos.dmg` |

```bash
cmake --build build --config Release && (cd build && cpack -C Release)
```

Unlike the Tauri binary, this app is not self-contained: it needs the frontend
assets and seven runtime DLLs. The Windows install rule discovers those with
`file(GET_RUNTIME_DEPENDENCIES)` rather than listing them, so a new transitive
dependency cannot be silently left out. Each platform gets the layout its
packager expects — beside the exe, `share/lectern`, or `Contents/Resources` —
and `src/assets.cpp` resolves the frontend relative to the running executable,
so the same binary works from a build tree, a portable zip, a .deb and an
.app bundle.

### Signing

You supply the credentials; nothing here fabricates them. `scripts/sign-windows.ps1`
drives `signtool` from either a certificate thumbprint (a hardware token or
Azure Trusted Signing — what a code-signing certificate issued after June 2023
requires) or a `.pfx`, always with a timestamp so signatures outlive the
certificate. `scripts/sign-macos.sh` codesigns with the hardened runtime,
notarizes through `notarytool` and staples the ticket.

Both scripts refuse to run without real credentials rather than falling back to
a self-signed or ad-hoc signature. That is deliberate: an ad-hoc macOS signature
fails notarization and a self-signed Windows certificate is treated exactly like
no signature — either would only *look* signed while shipping the same warning
to users.

Linux has no equivalent, so the release job publishes `SHA256SUMS` with a
detached GPG signature.

Every signing secret is optional. Without them CI still produces working,
unsigned installers, with a warning annotation on the run.

### Updater

The Tauri build had none — users reinstalled from the web app's Download page.
This adds the check, on the model Tauri's own updater uses: a small JSON
manifest, an Ed25519 signature over its exact bytes, and the public key compiled
into the binary. A manifest whose signature fails is discarded *before* it is
parsed, so a hostile one never reaches the JSON reader. The downloaded installer
is then checked against the SHA-256 in the signed manifest, which is what binds
the signature to the actual bytes wherever they were served from.

A build with no key compiled in cannot check for updates at all — that is the
default, and the test suite asserts it.

Installing is handed to the platform's own installer rather than replacing the
running binary: the NSIS setup on Windows, the .dmg on macOS, and on Linux
nothing at all, since the package manager owns the install. Inside Flatpak the
check is skipped entirely.

The app has no update UI because the frontend is shared with the web app and
knows nothing about updates, so the prompt is a native dialog.

```bash
./build/Release/lectern-updater-tool keygen
```

The public half goes into the build (`-DLECTERN_UPDATE_PUBLIC_KEY=…`), the
secret half into a CI secret and nowhere else. Rotating it strands every
installed copy, which is why it is worth generating once and backing up.

### Flatpak

`flatpak/com.lectern.app.yml` repackages the published `.deb`, the same approach
the Tauri manifest used — the dependency tree comes from vcpkg, which does not
exist inside a Flatpak build sandbox. Update the `url` and `sha256` per release;
a wrong digest fails the build rather than shipping the wrong binary.

It needs **less** sandbox access than the Tauri version. That manifest required
`--filesystem=home` because rfd's GTK3 backend was not a portal, so it could
only see paths already mounted in. This build uses `GtkFileDialog`, which routes
through `xdg-desktop-portal` automatically inside a sandbox, so the user's chosen
folder is granted individually.

That change earns its keep outside Flatpak too. The dialogs were shelling out to
zenity, which does not exist in `org.gnome.Platform` — the app would have been
unable to pick a work folder at all inside the sandbox. GTK 4 is already linked
into the process by WebKitGTK, so using it directly costs nothing; zenity and
kdialog remain as a fallback when GTK is absent at build time.

### Releasing

`.github/workflows/release.yml` is the whole pipeline. Tag and push:

```bash
git tag v0.13.0 && git push origin v0.13.0
```

That builds Windows, Linux and macOS in parallel, runs the tests, signs
whatever there are credentials for, and publishes a GitHub Release containing
every installer, `SHA256SUMS`, and the signed `latest.json` the in-app updater
reads. A `workflow_dispatch` run does everything except publish, attaching the
installers to the run as artifacts instead — which is how to check a change to
the packaging without burning a version number.

The tag is the version: it is passed as `-DLECTERN_VERSION`, which sets the
CPack artifact names, the Windows version resource, and the number the updater
compares against a manifest. Nothing else needs editing to cut a release. (Get
this wrong and the symptom is nasty: a build that believes it is the old
version re-offers the update forever, because installing it never changes the
number it compares.)

Two things worth knowing before the first run. saucer's vcpkg port depends on
vcpkg's own `gtk`, so the first Linux build compiles GTK from source and is
slow; vcpkg binaries are cached into the Actions cache, so later runs reuse
them. And the Windows job builds twice — once dynamically for the installers,
once statically for the portable exe — which the same cache absorbs.

## When cloud sync won't connect

`lectern-cloud-probe` runs the same code path `cloud_connect` runs — same HTTP
client, same endpoints, same order — outside the webview, printing each step:

```bash
./build/Release/lectern-cloud-probe http://your-server:8080 you@example.com yourpassword
```

It distinguishes the cases the in-app alert cannot: server unreachable, wrong
credentials (401), not a Lectern server (404), rate-limited (429), or a login
that succeeds but returns no API token.

### 413 on the first sync

The most likely cause, and the one that bit this project: the server refuses a
file as too large. Lectern's `.mdn` notebooks carry freehand ink, imported PDF
pages and images, so they pass a few megabytes easily — and the Rust server's
workspace route inherited axum's **2 MiB** `DefaultBodyLimit`, which nothing
overrode. Raise it server-side with `MAX_WORK_FILE_BYTES` (32 MiB by default
now) and redeploy; if the server is behind nginx, its `client_max_body_size`
has to clear the same bar.

The sync engine no longer treats this as fatal. A file the server rejects is
logged, named, and skipped; everything else still syncs and the connection
still succeeds. `cloud_status` carries the list as `syncErrors`, and an
explicit "Sync now" reports it. Before, the first rejected file aborted the
whole pass and rolled the connection back, so one oversized notebook made
cloud sync unusable with no indication of which file was to blame.

### Other causes

The most common of the rest is not a network problem at all. `cloud_connect` refuses a work folder
that was not picked through the OS dialog *in this build* — the folder-picker is
the trust boundary, and trusted folders are recorded in `authorized_roots.json`
under the data directory. Two consequences worth knowing:

- A folder picked in the Tauri build is not trusted here; the two keep separate
  config directories.
- A folder picked in the installed build is not trusted by the **portable**
  build, and vice versa, because a portable build deliberately keeps its state
  beside the executable. Pick the folder again in whichever build you are
  running, or point both at one place with `$LECTERN_DATA_DIR`.

## Multiple windows

Dragging a tab past the edge of the window tears it off into a new one, and
dropping it on another Lectern window moves it there. Both halves need the
backend to be able to create a window, which the Tauri build does through
`new WebviewWindow(label, options)`.

Nothing here provided that, so the frontend fell through to `window.open()` —
which a WebView2 host ignores unless it handles `NewWindowRequested`. Dragging
a tab out did nothing at all, with no error anywhere. `src/window_manager.cpp`
now creates real windows and `tauri_shim.h` presents the `WebviewWindow` API
the frontend already calls, so the existing frontend path works unchanged.

Three details that matter:

- **One storage profile for every window.** The frontend coordinates windows
  through `localStorage` and its `storage` events, which only reach documents
  in the same origin *and* the same profile. Give each window its own profile
  and they stop seeing each other entirely, so the path is decided once in
  `window_manager.cpp` rather than per window.
- **Placement goes through the native handle.** saucer 6 has no
  `set_position`, and a tear-off that ignores where you dropped it feels
  broken, so the window is moved with `SetWindowPos` on its HWND.
- **The file is `window_manager.h`, not `windows.h`.** A header called
  `windows.h` on the target's include path shadows the Windows SDK's, and
  every downstream translation unit then fails deep inside `rpcasync.h` with
  errors naming neither file.

Closing the last window quits the application; closing any other just drops it
from the registry.

## Portable builds

Each platform already has a portable format, and the release produces all
three:

| Platform | Portable artifact | What it is |
|---|---|---|
| Windows | `Lectern-<v>-windows-x64-portable.exe` | one file, nothing else |
| Windows | `Lectern-<v>-windows-x64.zip` | a folder you can copy |
| Linux | `Lectern-<v>-linux-x86_64.AppImage` | one file, `chmod +x` and run |
| macOS | `Lectern.app` inside the `.dmg` | drag anywhere |

The Windows single-file build is the interesting one, because Windows has no
portable convention of its own. Two things make it work:

- **`-DLECTERN_EMBED_FRONTEND=ON`** packs the frontend
  (`tools/pack_frontend.cpp`) and links the blob into the binary — an RCDATA
  resource on Windows, `.incbin` elsewhere. Both are free at compile time; a
  generated `.cpp` full of array literals would mean a compiler parsing tens of
  megabytes. `src/embedded.cpp` reads it back, bounds-checking every offset
  against the real blob size so a truncated binary fails closed.
- **`-DVCPKG_TARGET_TRIPLET=x64-windows-static`** with
  `-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded` links the C runtime and every
  library in, including WebView2Loader.

Build it yourself:

```bash
cmake -S . -B build-portable -G "Visual Studio 17 2022" -A x64 -DCMAKE_TOOLCHAIN_FILE=E:/vcpkg/scripts/buildsystems/vcpkg.cmake -DVCPKG_TARGET_TRIPLET=x64-windows-static -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded -DLECTERN_EMBED_FRONTEND=ON -DBUILD_TESTING=OFF
```

```bash
cmake --build build-portable --config Release
```

The static triplet needs its own dependency set, which vcpkg builds once:

```bash
vcpkg install saucer:x64-windows-static cpr:x64-windows-static nlohmann-json:x64-windows-static libsodium:x64-windows-static spdlog:x64-windows-static
```

The result is `build-portable/Release/lectern.exe` — 9.1 MB, importing nothing
but Windows system DLLs. Copy it anywhere and run it.

State goes in a `lectern-data` folder **beside the executable**, not in
`%APPDATA%`, so moving the .exe takes the workspace registry, the cloud-sync
config and the webview profile with it. `$LECTERN_DATA_DIR` overrides that if
you want it somewhere specific. Non-portable builds keep using the per-user
config directory, unchanged.

One caveat: the app still needs the **WebView2 runtime**, which is not
something any build flag can remove — it is the browser engine, it ships with
Windows 10 and 11, and Tauri had exactly the same requirement.

## Verified — packaging and portable builds

Beyond the port itself:

- The portable zip builds and contains the exe, all seven DLLs and 41 frontend
  files — 10.5 MB
- The install tree runs standalone with no environment variables: the frontend
  resolves relative to the executable and the full UI renders
- `lectern-updater-tool` round-trips: keygen → sign → verify, with a tampered
  manifest and a wrong key both rejected
- `scripts/make-update-manifest.sh` produces a valid signed manifest with
  correct digests, and verifies it before publishing
- The updater's failure path: pointed at an unreachable host, it logs at debug,
  does not prompt, and leaves the app running
- 78 assertions, including forged-signature and malformed-key rejection
- The Flatpak manifest and the release workflow both parse

On the portable build specifically:

- `lectern.exe` alone in an empty folder starts and renders the full UI, with
  all 41 frontend files served from inside the binary
- `dumpbin /DEPENDENTS` shows only Windows system DLLs — no WebView2Loader, no
  cpr, curl, sodium, spdlog, fmt or zlib
- Launched from a different working directory, it writes its state beside the
  executable and leaves the working directory untouched
- `-DLECTERN_VERSION=0.13.0` flows through to the CPack artifact name

Not verified here: the NSIS and WiX generators (neither toolchain is installed
on this machine — CI installs NSIS and the GitHub runners ship WiX), the
AppImage and .deb/.rpm builds, and all signing and notarization, which need real
certificates and the target platforms.
