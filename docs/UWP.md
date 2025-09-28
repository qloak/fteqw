# Experimental WinRT/UWP Notes

FTE now ships with a dedicated WinRT system layer in
`engine/client/sys_winrt.cpp`.  The module replaces the old stub inside
`sys_win.c` and provides a minimal but functional bridge between the engine and
the Windows Runtime so that the project can be compiled – and partially run –
under the Windows Store toolset without triggering desktop-only API checks.

> ⚠️ **Status:** the engine is **not** playable on UWP yet.  Window creation,
> input, audio, swap-chain handling, threading and sandbox storage still need
> dedicated implementations before an Xbox Dev Mode build can boot.

## Configuring a UWP build

1. Install Visual Studio 2022 (or newer) with the "Universal Windows Platform
   development" workload and the latest Windows 10/11 SDK.
2. Configure the project with the bundled preset, e.g.:

   ```powershell
   cmake --preset uwp-x64
   cmake --build --preset uwp-x64-debug
   ```

   The presets automatically set `CMAKE_SYSTEM_NAME=WindowsStore`, inject the
   `WINRT` define for C/C++ sources and mark the toolchain as cross-compiling so
   CMake does not attempt to run host binaries.
3. If you prefer to stay inside the legacy makefile workflow, the same presets
   can be driven through `make FTE_TARGET=uwp gl-rel` (release) or `gl-dbg`
   (debug), which proxy to the preset-aware `cmake --build` invocation.
4. Open the generated solution in Visual Studio and choose the `Debug`/`Release`
   configuration paired with the desired architecture (x64, ARM64, …).  At this
   point the project should compile far enough to surface the remaining
   platform TODOs.

## What now works

* Logging, warnings and fatal errors flow to both the Visual Studio output
  window and `stderr`, mirroring the desktop developer experience while running
  inside the sandbox.
* `Sys_Init` brings up a single-threaded WinRT apartment, captures the
  `CoreWindow` for the current view and keeps its `IUnknown` alive so
  `vid_d3d11.c` can create a `SwapChainForCoreWindow`.  Resize and DPI change
  notifications are forwarded to `D3D11_DoResize`, keeping the renderer in sync
  with the window in physical pixels.
* `Sys_SendKeyEvents` processes the CoreDispatcher event queue every frame so
  input backends can attach to WinRT messages once they are implemented.
* CoreWindow visibility and activation now drive `vid.activeapp`/`vid.isminimized`
  and gate audio via `S_BlockSound`/`S_UnblockSound`, so the engine idles and
  mutes correctly when the app is backgrounded on Xbox.
* Suspend/resume notifications from `CoreApplication` trim the D3D11 device,
  release swap-chain render targets, and rebuild the backbuffer on resume while
  queuing a `vid_restart` if the GPU is removed.  This keeps presentation happy
  across the WinRT lifecycle instead of crashing on background transitions.
* High resolution timers (`Sys_Milliseconds`, `Sys_DoubleTime`) rely on
  `QueryPerformanceCounter`, matching the cadence of the desktop build and
  avoiding the jitter introduced by `GetTickCount`.
* File-system helpers (`Sys_mkdir`, `Sys_remove`, `Sys_rmdir`, `Sys_Rename`)
  resolve paths into `ApplicationData::Current().LocalFolder()` and run the
  underlying WinRT async APIs through synchronous wrappers.  When the engine
  tries to open content outside the sandbox the runtime now drives a
  `FileOpenPicker`, copies the selection into an `ImportedContent/` cache, and
  remembers the mapping so PAKs and loose assets can be staged for the next
  boot without tripping capability errors.  `Sys_Init` also points
  `host_parms.basedir` at the sandbox `AppData\\LocalState\\games\\`
  directory (and `host_parms.binarydir` at the package install location),
  creating the `games` folder on first boot so startup immediately scans the
  packaged game-data tree instead of relying on Desktop Bridge working-directory
  shims.
* `Sys_LoadLibrary` now calls `LoadPackagedLibrary` so helper DLLs that ship in
  the package can be discovered, and `Sys_RandomBytes` pulls entropy from the
  system RNG via `BCryptGenRandom`.
* The Direct3D 11 swap-chain path now presents through `IDXGISwapChain1::Present1`
  so it follows the Windows Store submission rules and clamps the sync interval
  to the values accepted on WinRT. It also recreates the renderer when DXGI
  reports device removal, waits on the frame-latency waitable object to throttle
  presentation, and logs DXGI frame statistics when `r_speeds` is verbose so you
  can spot timing issues while testing.
* The networking layer now uses WinRT `DatagramSocket` instances to back the
  standard UDP transport and WinRT `StreamSocket` connections for TCP clients
  such as HTTP downloads or remote console. This removes the loopback-only
  stub so IPv4 multiplayer works again while still registering loopback sockets
  for bot matches and LAN testing. IPv6 listeners resolve and bind correctly
  now, TLS connections use the system certificate store via
  `SocketProtectionLevel::Tls12`, and client WebSocket streams ride on
  WinRT's `MessageWebSocket` so HTTPS master lists and RTC brokers work again.

## Packaging and sideloading

The WinRT toolchain produces loose binaries in your build directory. Microsoft
expects Windows Store submissions – and Xbox Dev Mode sideloads – to ship as
signed AppX/MSIX bundles. The repository now includes
`cmake/uwp/AppxManifest.template.xml` that can be copied and customised for your
package identity, publisher information, and logos. A minimal workflow looks
like this:

1. Build the project with the supplied preset:

   ```powershell
   cmake --preset uwp-x64
   cmake --build --preset uwp-x64-release
   ```

2. Create a staging directory (for example `build/uwp-package/AppLayout/`) and
   copy the generated binaries (`fteqw.exe`, content packs, and the
   WinRT-specific DLLs) into it. Assets referenced by the manifest (logos, audio
   cues, etc.) must live under the same root.  Mirror your game content under
   `AppData\LocalState\games\...` inside that layout (for example
   `AppData\LocalState\games\id1\pak0.pak`) so the engine finds Quake data in
   the sandbox on first boot without relying on the runtime file picker.  The
   repository ships `.fmf` manifests in `games/`, so copying them into
   `PackageLayout/AppData/LocalState/games/` keeps the presets available too.

3. Copy `cmake/uwp/AppxManifest.template.xml` into the staging directory as
   `AppxManifest.xml` and update the `<Identity>`, `<PublisherDisplayName>`,
   `<VisualElements>`, and `<Executable>` fields so they match your build and
   certificate.  The template is already trimmed to Xbox-safe capabilities, so
   avoid reintroducing restricted declarations such as `runFullTrust`.

4. Run the Windows SDK tools to create and sign the bundle:

   ```powershell
   makeappx pack /d build\uwp-package\AppLayout /p FTEQuake-UWP.appx
   signtool sign /fd SHA256 /a /f MyDevCert.pfx FTEQuake-UWP.appx
   ```

   The certificate must be trusted on the target device (install it through the
   Xbox Device Portal or the Windows certificate store before sideloading).

5. Deploy the signed package either through Visual Studio's *Device Portal*
   integration or with the Device Portal web UI on Xbox Dev Mode. After
   installation, the title appears under *My Games & Apps → Games*.

These steps match the Store certification toolchain, so once the remaining
runtime work is complete you can reuse the same manifest and signing process to
submit the package.

## Outstanding work before the port runs

* Run the updated Direct3D 11 lifecycle through the Windows Store
  certification tooling and surface the telemetry inside developer diagnostics so
  regressions are caught before packaging.
* Produce an AppX/MSIX packaging workflow and document the steps needed to
  deploy the build through the Xbox Device Portal.
* Build out automated packaging helpers (PowerShell or CMake targets) so
  developers do not need to manually mirror the `makeappx` commands above.

This document will evolve as additional pieces of the port land.  Contributions
that replace the placeholders above are welcome—just keep the sandbox
restrictions in mind when adding new APIs.
