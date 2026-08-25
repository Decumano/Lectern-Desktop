# Lectern

A Markdown-native office suite for worldbuilding, packaged as a Tauri desktop app
for Windows, Linux and macOS.

Documents, spreadsheets, diagrams, a drawing notebook and four structured
worldbuilding databases — all writing plain text files into a folder you choose
and control. No project database, no proprietary container, no lock-in: the
workspace is a directory of readable files you can back up with git, sync with
anything, and open in any editor.

There is also a self-hosted multi-user web port,
[officesuite-web](https://github.com/Decumano/officesuite-web), which runs the
same frontend against a Rust/axum server with accounts instead of a Tauri
backend.

## The apps

| App | Extension | What it is |
|---|---|---|
| **Documents** | `.mdp` | Markdown editor with live preview, math, diagrams, alignment and font blocks, field cards and tags |
| **Sheets** | `.mds` | Spreadsheet — 79 functions, multiple tabs, merged cells, cell colors, Chart.js charts |
| **Diagrams** | `.mdg` | Mermaid editor with a live-rendered canvas and a template gallery |
| **Notebook / Maps** | `.mdn` | Freehand ink, shapes and text boxes on paged canvases; PDF import and annotation; SVG import; world-map pages with linked pins, a scale reference and a travel-time calculator |
| **Glossary** | `.mdl` | Terms and constructed-language root words |
| **Calendar** | `.mdc` | Custom calendar systems — arbitrary months, seasons and holidays, anchored to a real date |
| **Economy** | `.mde` | Currencies, exchange rates, trade goods and regions |
| **Bestiary** | `.mdb` | Creature entries |

Each app can be switched off in Settings, so a workspace used for plain writing
does not have to show a bestiary.

## Features

**Editing**

- Live Markdown preview with math (KaTeX), Mermaid diagrams, `[TOC]`,
  roman-numeral lists, alignment and font blocks, metadata field cards and live
  task-list checkboxes — see [FLAVOR.md](src/FLAVOR.md)
- Find and replace, native OS spell checking with a per-document language, custom
  fonts, custom document templates
- Undo/redo that batches a burst of typing into one step

**Navigating a large workspace**

- Global search across every file, including files not yet loaded
- Backlinks — automatically derived from name mentions, stored out-of-band so
  files are never modified to record them
- `#tag` pills that filter the sidebar
- Cross-file links and live embeds: drop a diagram or a sheet's charts straight
  into a document, rendered from the target file's current contents

**Window management**

- Browser-style tabs for open files
- Split view, detachable windows, and drag-and-drop of a tab into the side pane,
  a new window or an existing one
- Collapsible sidebar, plus an off-canvas drawer on narrow screens

**Files**

- Pick any folder on disk as the workspace; files, folders, renames and moves are
  real filesystem operations
- Export to HTML or PDF; import existing files
- Themes and appearance settings that roam with the workspace

## Repository layout

```
src/          frontend — HTML/CSS/JS, no build step (git submodule)
src-tauri/    Rust desktop backend: window, dialogs, filesystem commands
```

The frontend lives in its own repo,
[officesuite-frontend](https://github.com/Decumano/officesuite-frontend), because
it is shared with the web port. It talks to a backend only through the `Platform`
adapter in `src/platform.js` — `main.js` never calls Tauri or HTTP directly.
`src-tauri/src/lib.rs` is the reference implementation of the commands that
adapter expects (`pick_work_folder`, `read_work_file`, `write_work_file`,
`list_work_folder`, `create_work_folder`, `delete_work_entry`, `move_work_entry`,
`save_file`, `defaultFile`).

## Getting the code

Clone with submodules included:

```
git clone --recurse-submodules https://github.com/Decumano/OfficeSuite.git
```

Or, if you already have a plain clone:

```
git submodule update --init --recursive
```

## Building and running

Requires the [Rust toolchain](https://www.rust-lang.org/tools/install), Node.js
(for the Tauri CLI only — the frontend itself has no build step) and the
[Tauri v2 system prerequisites](https://tauri.app/start/prerequisites/) for your
platform.

```
npm install
npm run tauri dev
```

To produce installers for the current platform:

```
npm run tauri build
```

Because the frontend is plain static files, editing anything under `src/` takes
effect on reload — no bundler, no watch step.

## Releasing desktop builds

Pushing a version tag builds the app for Windows, Linux and macOS and publishes
installers (plus a portable Windows zip) as a GitHub Release
(see `.github/workflows/release.yml`):

```
git tag v0.1.0
git push origin v0.1.0
```

The web app's Download page (`download.html` in the frontend) lists the assets of
the latest release automatically, so publishing a release is all that's needed for
downloads to appear on every officesuite-web instance. A manual workflow run
("Run workflow" in the Actions tab) builds everything without publishing,
attaching the bundles to the run as artifacts instead.

## File formats

Every format the suite writes is documented in [FLAVOR.md](src/FLAVOR.md) — the
Markdown extensions, the spreadsheet serialization, the notebook payload and the
JSON-backed data types. Documents and diagrams stay readable in any plain
Markdown viewer.

## Recommended IDE Setup

- [VS Code](https://code.visualstudio.com/) + [Tauri](https://marketplace.visualstudio.com/items?itemName=tauri-apps.tauri-vscode) + [rust-analyzer](https://marketplace.visualstudio.com/items?itemName=rust-lang.rust-analyzer)

## License

GPLv3 — see [LICENSE](LICENSE). The shared frontend in `src/` is GPLv3 as well.
The self-hosted server, [officesuite-web](https://github.com/Decumano/officesuite-web),
is **AGPLv3** instead, so that anyone running a modified public instance owes its
users their changes; the two licenses are compatible and the desktop app is
unaffected by that choice.

Third-party libraries vendored into the frontend keep their own licenses, listed
in [THIRD-PARTY-NOTICES.md](src/THIRD-PARTY-NOTICES.md).
