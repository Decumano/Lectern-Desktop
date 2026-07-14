# Tauri + Vanilla

This template should help get you started developing with Tauri in vanilla HTML, CSS and Javascript.

## Getting the code

The frontend (`src/`) lives in its own repo,
[officesuite-frontend](https://github.com/Decumano/officesuite-frontend), and is included here as
a git submodule. Clone with submodules included:

```
git clone --recurse-submodules https://github.com/Decumano/OfficeSuite.git
```

Or, if you already have a plain clone:

```
git submodule update --init --recursive
```

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
("Run workflow" in the Actions tab) builds everything without publishing, attaching
the bundles to the run as artifacts instead.

## Recommended IDE Setup

- [VS Code](https://code.visualstudio.com/) + [Tauri](https://marketplace.visualstudio.com/items?itemName=tauri-apps.tauri-vscode) + [rust-analyzer](https://marketplace.visualstudio.com/items?itemName=rust-lang.rust-analyzer)
