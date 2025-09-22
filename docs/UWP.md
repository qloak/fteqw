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
* High resolution timers (`Sys_Milliseconds`, `Sys_DoubleTime`) rely on
  `QueryPerformanceCounter`, matching the cadence of the desktop build and
  avoiding the jitter introduced by `GetTickCount`.
* File-system helpers (`Sys_mkdir`, `Sys_remove`, `Sys_rmdir`, `Sys_Rename`)
  resolve paths into `ApplicationData::Current().LocalFolder()` and operate on
  UTF-8 filenames through the Win32-on-UWP shims that remain available inside
  the sandbox.
* `Sys_LoadLibrary` now calls `LoadPackagedLibrary` so helper DLLs that ship in
  the package can be discovered, and `Sys_RandomBytes` pulls entropy from the
  system RNG via `BCryptGenRandom`.

## Outstanding work before the port runs

* Harden the Direct3D 11 path for the full UWP life-cycle (suspend/resume,
  visibility changes, device-loss) and hook `Present1`/`Validate` to obey Store
  submission rules.
* Replace the remaining Win32 threading, input and networking layers with
  WinRT-friendly equivalents (search for `WINRT` guards to locate the stubs).
* Harden the filesystem backends to cope with UWP's async-only APIs and expose
  file pickers for content outside the sandbox when necessary.
* Produce an AppX/MSIX packaging workflow and document the steps needed to
  deploy the build through the Xbox Device Portal.

This document will evolve as additional pieces of the port land.  Contributions
that replace the placeholders above are welcome—just keep the sandbox
restrictions in mind when adding new APIs.
