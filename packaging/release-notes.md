Desktop builds of Lectern.

- **Windows**: `-setup.exe` (installer, recommended), `.msi` for policy
  deployment, the `-portable.exe` (a single file, nothing to install), or the `.zip`
  folder build — unzip and run. All require the
  [WebView2 runtime](https://developer.microsoft.com/microsoft-edge/webview2/),
  preinstalled on Windows 10/11.
- **Linux**: `.AppImage`, the `-portable.tar.gz` (extract and run `./lectern`),
  `.deb`, or `.rpm`.
- **Portable builds** (Windows `-portable.exe`, Linux `-portable.tar.gz`) keep
  their settings in a `lectern-data` folder beside the program, so they can
  live on a USB stick.
- **macOS**: `.dmg`.

Verify a download against `SHA256SUMS` (and `SHA256SUMS.asc`, if present):

```
sha256sum -c SHA256SUMS --ignore-missing
```
