# Electron Demo Troubleshooting

## `Electron failed to install correctly`

If `electron --version` or `npm start` prints this, the prebuilt Electron binary did not download. Common fixes:

1. Re-run the download script explicitly:
   ```powershell
   cd demo/electron
   node node_modules/electron/install.js
   ```

2. If that still leaves `node_modules/electron/dist/` empty, delete and reinstall:
   ```powershell
   rm -Recurse -Force node_modules/electron
   npm install electron@^31.0.0
   ```

3. On networks that block GitHub releases, set an Electron mirror before installing:
   ```powershell
   $env:ELECTRON_MIRROR="https://npmmirror.com/mirrors/electron/"
   npm install electron@^31.0.0
   ```

4. If npm warns about `allow-scripts`/`pending install scripts`, allow them:
   ```powershell
   npm approve-scripts --allow-scripts-pending
   # or
   npm config set ignore-scripts false
   ```

## Native module rebuild fails

The WaveDB `.node` binary in `bindings/nodejs/build/Release/` is built for your system Node.js version, not for Electron. You must rebuild it for Electron's Node ABI.

Run the included rebuild helper from `demo/electron`:

```powershell
cd demo/electron
npm run rebuild
```

This resolves Electron's exact version and runs `node-gyp rebuild --target=<electron-version> --dist-url=https://electronjs.org/headers` inside `bindings/nodejs`.

If `node-gyp` is missing, install it globally:

```powershell
npm install -g node-gyp
```

## `The module was compiled against a different Node.js version`

This confirms the native binding was not rebuilt for Electron. Run `npm run rebuild` in `demo/electron`.

## Windows: `Cannot open include file: 'openssl/evp.h'`

The published `@vijayee/wavedb` package does not bundle OpenSSL headers, so the default Windows build disables OpenSSL (`WAVEDB_NO_OPENSSL`). This is enough for the demo.

If you want encryption support on Windows, install OpenSSL and opt in before installing:

```powershell
$env:WAVEDB_OPENSSL="1"
$env:OPENSSL_ROOT="C:\Program Files\OpenSSL-Win64"
npm install
```

Supported `OPENSSL_ROOT` layouts (auto-searched if `OPENSSL_ROOT` is not set):

- Shining Light Productions installer: `C:\OpenSSL-Win64` or `C:\Program Files\OpenSSL-Win64`
- vcpkg: `C:\vcpkg\installed\x64-windows`

## Windows: `'true' is not recognized`

This was a bug in earlier versions of the demo's `package.json` that used POSIX `|| true`. The current scripts are Windows-compatible and no longer produce this error.

## Still stuck?

Run the presentation in the browser instead. The same `index.html` uses in-memory mocks when opened directly:

```powershell
cd demo
start index.html
```
