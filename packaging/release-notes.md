Desktop builds of Lectern.

- **Windows**: `-setup.exe` (installer, recommended), `.msi` for policy
  deployment, or the `.zip` portable build — unzip and run. All require the
  [WebView2 runtime](https://developer.microsoft.com/microsoft-edge/webview2/),
  preinstalled on Windows 10/11.
- **Linux**: `.AppImage` (portable), `.deb`, or `.rpm`.
- **macOS**: `.dmg`.

Verify a download against `SHA256SUMS` (and `SHA256SUMS.asc`, if present):

```
sha256sum -c SHA256SUMS --ignore-missing
```
