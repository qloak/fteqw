# Experimental WinRT/UWP Notes

This repository ships with an (extremely) thin WinRT stub that can be used as a
starting point for a Universal Windows Platform port.  The stub lives inside
`engine/client/sys_win.c` behind the `WINRT` preprocessor guard and now avoids
most of the Win32-only APIs (file system, clipboard, console and library
loading are all reduced to inert placeholders).  As a result the code can be
compiled with the Windows Store toolset without tripping the store validation
that forbids classic Win32 entry points.

> ⚠️ **Status:** the engine is **not** playable on UWP yet.  Window creation,
> input, audio, swap-chain handling, threading and sandbox storage still need
> dedicated implementations before an Xbox Dev Mode build can boot.

## Configuring a UWP build

1. Install Visual Studio 2022 (or newer) with the "Universal Windows Platform
   development" workload and the latest Windows 10/11 SDK.
2. Generate a build directory that targets the Windows Store toolchain, for
   example:

   ```powershell
   cmake -S . -B build-uwp \
     -G "Visual Studio 17 2022" -A x64 \
     -DCMAKE_SYSTEM_NAME=WindowsStore \
     -DCMAKE_SYSTEM_VERSION=10.0
   ```
3. Force the UWP-oriented code path by defining `WINRT` for both C and C++
   sources.  With CMake you can inject the define via cache entries:

   ```powershell
   cmake --build build-uwp --config Debug \
     -- /p:C_FLAGS="/DWINRT" /p:CXX_FLAGS="/DWINRT"
   ```

   Alternatively, set `CMAKE_C_FLAGS`/`CMAKE_CXX_FLAGS` during the generation
   step if you prefer a persistent configuration.
4. Open the generated solution in Visual Studio and choose the `Debug`/`Release`
   configuration paired with the desired architecture (x64, ARM64, …).  At this
   point the project should compile far enough to surface the remaining
   platform TODOs.

## What the new stub covers

* Replaces Win32 file, clipboard and library calls with inert implementations
  that simply log to the Visual Studio output window/terminal.
* Provides deterministic timing through `QueryPerformanceCounter`, matching the
  desktop build so that higher level subsystems continue to function while the
  real UWP windowing layer is being developed.
* Supplies a basic (non-cryptographic) random byte generator so subsystems that
  expect `Sys_RandomBytes` do not crash during bring-up.

## Outstanding work before the port runs

* Implement a `CoreWindow`/`SwapChainPanel` presentation path for the D3D11
  renderer (`engine/d3d/vid_d3d11.c`).
* Replace the remaining Win32 threading, input, networking and audio backends
  with WinRT-friendly equivalents (see the `WINRT` stubs referenced throughout
  the tree via `rg WINRT`).
* Rework the filesystem layer to read/write inside `ApplicationData` and expose
  user-selectable folders via the UWP file pickers.
* Produce an AppX/MSIX packaging workflow and document the steps needed to
  deploy the build through the Xbox Device Portal.

This document will evolve as additional pieces of the port land.  Contributions
that replace the placeholders above are welcome—just keep the sandbox
restrictions in mind when adding new APIs.
