# Experimental WinRT/UWP Notes

This document covers the current state of the FTEQW Universal Windows Platform
bring-up. The port is still experimental and incomplete, but the foundations are
in place for further work.

---

## Quickstart (UWP)

```powershell
# Configure with preset
cmake --preset uwp-x64
cmake --build --preset uwp-x64-debug

# Or use the legacy makefile workflow
make FTE_TARGET=uwp gl-rel   # release
make FTE_TARGET=uwp gl-dbg   # debug

# Then open the solution in Visual Studio for deployment/debugging
```

At this point the project should compile, but runtime functionality is **very
limited** (see below).

---

## System Layer

FTE now ships with a dedicated WinRT system layer in
`engine/client/sys_winrt.cpp`. The module replaces the old stub inside
`sys_win.c` and provides a minimal but functional bridge between the engine and
the Windows Runtime so that the project can be compiled – and partially run –
under the Windows Store toolset without triggering desktop-only API checks.

The old stub under the `WINRT` preprocessor guard is still referenced for
remaining unported subsystems (filesystem, clipboard, console, library loading,
etc.), which are reduced to placeholders until full WinRT support is written.

> ⚠️ **Status:** the engine is **not** playable on UWP yet. Window creation,
> input, audio, swap-chain handling, threading and sandbox storage still need
> dedicated implementations before an Xbox Dev Mode build can boot.

---

## Configuring a UWP Build

1. Install Visual Studio 2022 (or newer) with the **Universal Windows Platform
   development** workload and the latest Windows 10/11 SDK.

2. Configure the project with the bundled preset, e.g.:

   ```powershell
   cmake --preset uwp-x64
   cmake --build --preset uwp-x64-debug
   ```

   The presets automatically:

   * set `CMAKE_SYSTEM_NAME=WindowsStore`
   * inject the `WINRT` define for C/C++ sources
   * mark the toolchain as cross-compiling (so CMake does not attempt to run host binaries)

   If you prefer the legacy makefile workflow, you can drive the same presets
   via:

   ```powershell
   make FTE_TARGET=uwp gl-rel   # release
   make FTE_TARGET=uwp gl-dbg   # debug
   ```

3. Open the generated solution in Visual Studio and choose the `Debug`/`Release`
   configuration paired with the desired architecture (x64, ARM64, …). The
   project should compile far enough to surface the remaining platform TODOs.

---

## What Now Works

* **Logging & errors**: messages flow to both the Visual Studio output window
  and `stderr`, mirroring the desktop developer experience while running inside
  the sandbox.
* **Initialization**: `Sys_Init` brings up a single-threaded WinRT apartment,
  captures the `CoreWindow` for the current view, and keeps its `IUnknown`
  alive so `vid_d3d11.c` can create a `SwapChainForCoreWindow`. Resize and DPI
  change notifications are forwarded to `D3D11_DoResize`.
* **Input events**: `Sys_SendKeyEvents` processes the CoreDispatcher event queue
  every frame so input backends can attach to WinRT messages once implemented.
* **Timers**: high-resolution timers (`Sys_Milliseconds`, `Sys_DoubleTime`)
  rely on `QueryPerformanceCounter`, avoiding jitter.
* **Filesystem helpers**: (`Sys_mkdir`, `Sys_remove`, `Sys_rmdir`, `Sys_Rename`)
  resolve paths into `ApplicationData::Current().LocalFolder()` and operate on
  UTF-8 filenames through the Win32-on-UWP shims.
* **Dynamic libraries**: `Sys_LoadLibrary` calls `LoadPackagedLibrary` so DLLs
  bundled in the package can be discovered.
* **Entropy**: `Sys_RandomBytes` pulls randomness from the system RNG via
  `BCryptGenRandom`.

---

## Outstanding Work Before the Port Runs

* Harden the Direct3D 11 path for the full UWP lifecycle (suspend/resume,
  visibility changes, device-loss) and hook `Present1`/`Validate` to obey Store
  submission rules.
* Replace remaining Win32 subsystems:

  * threading
  * input
  * networking
  * audio
* Harden the filesystem backends to cope with UWP's async-only APIs and expose
  file pickers for content outside the sandbox when necessary.
* Produce an AppX/MSIX packaging workflow and document deployment steps through
  the Xbox Device Portal.

---

## Contributing

This document will evolve as additional pieces of the port land. Contributions
that replace the placeholders above are welcome — just keep the sandbox
restrictions in mind when adding new APIs.