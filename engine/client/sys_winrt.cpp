/*
Copyright (C) 1996-1997 Id Software, Inc.

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
*/

#include "quakedef.h"
#include "winquake.h"
#include "fs.h"

#ifdef WINRT

#include <windows.h>
#include <bcrypt.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <mutex>
#include <string>
#include <string_view>

#include <winrt/base.h>
#include <winrt/Windows.ApplicationModel.Core.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Display.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.ViewManagement.h>

extern "C" {
void D3D11_DoResize(int newwidth, int newheight);
}

using namespace winrt;
using namespace Windows::ApplicationModel::Core;
using namespace Windows::Foundation;
using namespace Windows::Graphics::Display;
using namespace Windows::Storage;
using namespace Windows::UI::Core;

typedef winrt::com_ptr<IUnknown> UnknownPtr;

typedef winrt::Windows::UI::Core::CoreWindow WinRTCoreWindow;

typedef winrt::Windows::UI::Core::WindowSizeChangedEventArgs WinRTWindowSizeChangedEventArgs;

qboolean isDedicated = false;

static std::once_flag g_runtimeInit;
static std::mutex g_logMutex;
static LARGE_INTEGER g_timeFreq = {};
static LARGE_INTEGER g_timeStart = {};
static std::atomic<bool> g_timeReady{false};
static std::atomic<int> g_cachedWidth{1280};
static std::atomic<int> g_cachedHeight{720};
static std::atomic<double> g_cachedScale{1.0};

static WinRTCoreWindow g_coreWindow{nullptr};
static UnknownPtr g_coreWindowUnknown;
static winrt::event_revoker<WinRTCoreWindow> g_sizeChangedRevoker;
static DisplayInformation g_displayInformation{nullptr};
static winrt::event_revoker<DisplayInformation> g_dpiChangedRevoker;

static std::wstring g_localStatePath;

static char *g_clipboardText = nullptr;

static void SysWinRT_DebugOutput(std::string_view message)
{
        std::string owned(message);
        if (!owned.empty() && owned.back() == '\n')
                owned.pop_back();
        owned.append("\n");
        OutputDebugStringA(owned.c_str());
        fputs(owned.c_str(), stderr);
        fflush(stderr);
}

static void SysWinRT_Logv(const char *prefix, const char *fmt, va_list args)
{
        char formatted[2048];
        Q_vsnprintfz(formatted, sizeof(formatted), fmt, args);

        std::string text;
        if (prefix && *prefix)
                text.append(prefix);
        text.append(formatted);

        std::lock_guard<std::mutex> guard(g_logMutex);
        SysWinRT_DebugOutput(text);
}

static std::wstring SysWinRT_ToWide(const char *utf8)
{
        if (!utf8 || !*utf8)
                return std::wstring();
        int required = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
        if (required <= 0)
                return std::wstring();
        std::wstring wide(static_cast<size_t>(required - 1), L'\0');
        if (!wide.empty())
                MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wide.data(), required);
        return wide;
}

static void SysWinRT_NormalizeSlashes(std::wstring &path)
{
        for (auto &ch : path)
        {
                if (ch == L'/')
                        ch = L'\\';
        }
}

static bool SysWinRT_IsAbsolutePath(const std::wstring &path)
{
        if (path.empty())
                return false;
        if (path.size() >= 2 && path[1] == L':')
                return true;
        if (path.size() >= 2 && path[0] == L'\\' && path[1] == L'\\')
                return true;
        return false;
}

static std::wstring SysWinRT_Combine(const std::wstring &base, const std::wstring &suffix)
{
        if (base.empty())
                return suffix;
        std::wstring result = base;
        if (!result.empty() && result.back() != L'\\')
                result.push_back(L'\\');
        result.append(suffix);
        return result;
}

static std::wstring SysWinRT_ResolvePath(const char *utf8)
{
        std::wstring candidate = SysWinRT_ToWide(utf8);
        if (candidate.empty())
                return std::wstring();
        SysWinRT_NormalizeSlashes(candidate);
        if (SysWinRT_IsAbsolutePath(candidate))
                return candidate;
        if (!g_localStatePath.empty())
                return SysWinRT_Combine(g_localStatePath, candidate);
        return candidate;
}

static double SysWinRT_ResolveScale(DisplayInformation const &info)
{
        double scale = 1.0;
        if (!info)
                return scale;
        try
        {
                scale = info.RawPixelsPerViewPixel();
        }
        catch (const winrt::hresult_error &)
        {
        }
        if (scale <= 0.0)
        {
                try
                {
                        scale = info.LogicalDpi() / 96.0;
                }
                catch (const winrt::hresult_error &)
                {
                        scale = 1.0;
                }
        }
        if (scale <= 0.0)
                scale = 1.0;
        return scale;
}

static void SysWinRT_UpdateScale(DisplayInformation const &info)
{
        double scale = SysWinRT_ResolveScale(info);
        g_cachedScale.store(scale);
}

static void SysWinRT_UpdateCachedSize(double widthDips, double heightDips)
{
        double scale = g_cachedScale.load();
        if (scale <= 0.0)
                scale = 1.0;
        double widthPixels = widthDips * scale;
        double heightPixels = heightDips * scale;
        int pixelWidth = static_cast<int>(std::lround(widthPixels));
        int pixelHeight = static_cast<int>(std::lround(heightPixels));
        if (pixelWidth <= 0)
                pixelWidth = static_cast<int>(std::lround(widthDips));
        if (pixelHeight <= 0)
                pixelHeight = static_cast<int>(std::lround(heightDips));
        if (pixelWidth <= 0)
                pixelWidth = 1;
        if (pixelHeight <= 0)
                pixelHeight = 1;
        g_cachedWidth = pixelWidth;
        g_cachedHeight = pixelHeight;
}

static void SysWinRT_AttachWindow(WinRTCoreWindow const &window)
{
        if (!window)
                return;
        g_coreWindow = window;
        try
        {
                g_coreWindowUnknown = window.as<IUnknown>();
        }
        catch (const winrt::hresult_error &)
        {
                g_coreWindowUnknown = nullptr;
        }

        try
        {
                auto display = DisplayInformation::GetForCurrentView();
                if (display)
                {
                        g_displayInformation = display;
                        SysWinRT_UpdateScale(display);
                        g_dpiChangedRevoker = display.DpiChanged(winrt::auto_revoke,
                                [](DisplayInformation const &sender, IInspectable const &)
                                {
                                        SysWinRT_UpdateScale(sender);
                                        if (g_coreWindow)
                                        {
                                                auto bounds = g_coreWindow.Bounds();
                                                SysWinRT_UpdateCachedSize(bounds.Width, bounds.Height);
                                                D3D11_DoResize(g_cachedWidth.load(), g_cachedHeight.load());
                                        }
                                });
                }
        }
        catch (const winrt::hresult_error &)
        {
        }

        auto bounds = window.Bounds();
        SysWinRT_UpdateCachedSize(bounds.Width, bounds.Height);

        g_sizeChangedRevoker = window.SizeChanged(winrt::auto_revoke,
                [](WinRTCoreWindow const &, WinRTWindowSizeChangedEventArgs const &args)
                {
                        auto size = args.Size();
                        SysWinRT_UpdateCachedSize(size.Width, size.Height);
                        D3D11_DoResize(g_cachedWidth.load(), g_cachedHeight.load());
                });
}

static WinRTCoreWindow SysWinRT_TryGetWindow()
{
        if (g_coreWindow)
                return g_coreWindow;
        try
        {
                auto window = WinRTCoreWindow::GetForCurrentThread();
                if (window)
                        SysWinRT_AttachWindow(window);
        }
        catch (const winrt::hresult_error &)
        {
                try
                {
                        auto view = CoreApplication::MainView();
                        auto window = view.CoreWindow();
                        if (window)
                                SysWinRT_AttachWindow(window);
                }
                catch (const winrt::hresult_error &)
                {
                }
        }
        return g_coreWindow;
}

static void SysWinRT_InitRuntime()
{
        try
        {
                winrt::init_apartment();
        }
        catch (const winrt::hresult_error &)
        {
        }

        try
        {
                auto data = ApplicationData::Current();
                g_localStatePath = data.LocalFolder().Path().c_str();
        }
        catch (const winrt::hresult_error &)
        {
                g_localStatePath.clear();
        }

        SysWinRT_TryGetWindow();

        QueryPerformanceFrequency(&g_timeFreq);
        QueryPerformanceCounter(&g_timeStart);
        g_timeReady = true;
}

static void SysWinRT_EnsureRuntime()
{
        std::call_once(g_runtimeInit, SysWinRT_InitRuntime);
}

void VARGS Sys_Error(const char *error, ...)
{
        va_list argptr;
        va_start(argptr, error);
        SysWinRT_Logv("Sys_Error: ", error, argptr);
        va_end(argptr);

        Host_Shutdown();
        try
        {
                CoreApplication::Exit();
        }
        catch (const winrt::hresult_error &)
        {
        }
        abort();
}

void VARGS Sys_Warn(char *fmt, ...)
{
        va_list argptr;
        va_start(argptr, fmt);
        SysWinRT_Logv("Sys_Warn: ", fmt, argptr);
        va_end(argptr);
}

void VARGS Sys_Printf(char *fmt, ...)
{
        va_list argptr;
        va_start(argptr, fmt);
        SysWinRT_Logv("", fmt, argptr);
        va_end(argptr);
}

void Sys_ServerActivity(void)
{
}

void Sys_RecentServer(char *command, char *target, char *title, char *desc)
{
        (void)command;
        (void)target;
        (void)title;
        (void)desc;
}

qboolean Sys_InitTerminal(void)
{
        return false;
}

char *Sys_ConsoleInput(void)
{
        return NULL;
}

void Sys_CloseTerminal(void)
{
}

dllhandle_t *Sys_LoadLibrary(const char *name, dllfunction_t *funcs)
{
        if (!name)
                return NULL;
        std::wstring wide = SysWinRT_ToWide(name);
        if (wide.empty())
                return NULL;
        HMODULE mod = LoadPackagedLibrary(wide.c_str(), 0);
        if (!mod)
                return NULL;
        if (funcs)
        {
                size_t i;
                for (i = 0; funcs[i].name; ++i)
                {
                        void *addr = (void *)GetProcAddress(mod, funcs[i].name);
                        if (!addr)
                                break;
                        *funcs[i].funcptr = addr;
                }
                if (funcs[i].name)
                {
                        FreeLibrary(mod);
                        return NULL;
                }
        }
        return (dllhandle_t *)mod;
}

void *Sys_GetAddressForName(dllhandle_t *module, const char *exportname)
{
        if (!module || !exportname)
                return NULL;
        return (void *)GetProcAddress((HMODULE)module, exportname);
}

void Sys_CloseLibrary(dllhandle_t *lib)
{
        if (lib)
                FreeLibrary((HMODULE)lib);
}

void Sys_Init(void)
{
        SysWinRT_EnsureRuntime();
        Sys_Printf("WinRT runtime initialised\n");
}

void Sys_Shutdown(void)
{
        if (g_sizeChangedRevoker)
        {
                try
                {
                        g_sizeChangedRevoker.revoke();
                }
                catch (const winrt::hresult_error &)
                {
                }
        }
        if (g_dpiChangedRevoker)
        {
                try
                {
                        g_dpiChangedRevoker.revoke();
                }
                catch (const winrt::hresult_error &)
                {
                }
        }
        g_displayInformation = nullptr;
        if (g_clipboardText)
        {
                Z_Free(g_clipboardText);
                g_clipboardText = NULL;
        }
        try
        {
                winrt::uninit_apartment();
        }
        catch (const winrt::hresult_error &)
        {
        }
}

qboolean Sys_RandomBytes(qbyte *string, int len)
{
        if (!string || len <= 0)
                return false;
        NTSTATUS status = BCryptGenRandom(NULL, (PUCHAR)string, len, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
        return status >= 0;
}

qboolean Sys_GetDesktopParameters(int *width, int *height, int *bpp, int *refreshrate)
{
        SysWinRT_TryGetWindow();
        if (width)
                *width = g_cachedWidth.load();
        if (height)
                *height = g_cachedHeight.load();
        if (bpp)
                *bpp = 32;
        if (refreshrate)
                *refreshrate = 60;
        return g_coreWindow != nullptr;
}

void INS_Move(void)
{
}

void INS_Commands(void)
{
}

void INS_Init(void)
{
}

void INS_ReInit(void)
{
}

void INS_Shutdown(void)
{
}

void INS_UpdateGrabs(int fullscreen, int activeapp)
{
        (void)fullscreen;
        (void)activeapp;
}

void *RT_GetCoreWindow(int *width, int *height)
{
        auto window = SysWinRT_TryGetWindow();
        if (!window)
                return NULL;
        if (width)
                *width = g_cachedWidth.load();
        if (height)
                *height = g_cachedHeight.load();
        if (g_coreWindowUnknown)
                return g_coreWindowUnknown.get();
        try
        {
                UnknownPtr tmp = window.as<IUnknown>();
                g_coreWindowUnknown = tmp;
                return g_coreWindowUnknown.get();
        }
        catch (const winrt::hresult_error &)
        {
        }
        return NULL;
}

static void SysWinRT_EnsureClock()
{
        if (!g_timeReady.load())
        {
                QueryPerformanceFrequency(&g_timeFreq);
                QueryPerformanceCounter(&g_timeStart);
                g_timeReady = true;
        }
}

unsigned int Sys_Milliseconds(void)
{
        SysWinRT_EnsureClock();
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        LONGLONG diff = now.QuadPart - g_timeStart.QuadPart;
        diff *= 1000;
        diff /= g_timeFreq.QuadPart;
        return (unsigned int)diff;
}

double Sys_DoubleTime(void)
{
        SysWinRT_EnsureClock();
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        LONGLONG diff = now.QuadPart - g_timeStart.QuadPart;
        return (double)diff / (double)g_timeFreq.QuadPart;
}

void Sys_Quit(void)
{
        Host_Shutdown();
        try
        {
                CoreApplication::Exit();
        }
        catch (const winrt::hresult_error &)
        {
        }
        exit(EXIT_SUCCESS);
}

static void SysWinRT_CreateDirectories(const std::wstring &path)
{
        if (path.empty())
                return;
        std::wstring partial;
        partial.reserve(path.size());
        for (size_t i = 0; i < path.size(); ++i)
        {
            wchar_t ch = path[i];
            partial.push_back(ch);
            if (ch == L'\\' || i + 1 == path.size())
            {
                    if (partial.size() <= 2 && partial.back() == L'\\')
                            continue;
                    CreateDirectoryW(partial.c_str(), NULL);
            }
        }
}

void Sys_mkdir(const char *path)
{
        std::wstring resolved = SysWinRT_ResolvePath(path);
        SysWinRT_CreateDirectories(resolved);
}

qboolean Sys_rmdir(const char *path)
{
        std::wstring resolved = SysWinRT_ResolvePath(path);
        if (resolved.empty())
                return false;
        if (RemoveDirectoryW(resolved.c_str()))
                return true;
        DWORD err = GetLastError();
        return err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND;
}

qboolean Sys_remove(const char *path)
{
        std::wstring resolved = SysWinRT_ResolvePath(path);
        if (resolved.empty())
                return false;
        if (DeleteFileW(resolved.c_str()))
                return true;
        DWORD err = GetLastError();
        return err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND;
}

qboolean Sys_Rename(const char *oldfname, const char *newfname)
{
        std::wstring from = SysWinRT_ResolvePath(oldfname);
        std::wstring to = SysWinRT_ResolvePath(newfname);
        if (from.empty() || to.empty())
                return false;
        if (MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED))
                return true;
        return false;
}

void Sys_Clipboard_PasteText(clipboardtype_t cbt, void (*callback)(void *cb, char *utf8), void *ctx)
{
        if (cbt != CBT_CLIPBOARD || !callback)
                return;
        callback(ctx, g_clipboardText ? g_clipboardText : (char *)"");
}

void Sys_SaveClipboard(clipboardtype_t cbt, char *text)
{
        if (cbt != CBT_CLIPBOARD)
                return;
        Z_Free(g_clipboardText);
        g_clipboardText = text ? Z_StrDup(text) : NULL;
}

void Sys_SendKeyEvents(void)
{
        auto window = SysWinRT_TryGetWindow();
        if (!window)
                return;
        try
        {
                auto dispatcher = window.Dispatcher();
                if (dispatcher)
                        dispatcher.ProcessEvents(CoreProcessEventsOption::ProcessAllIfPresent);
        }
        catch (const winrt::hresult_error &)
        {
        }
}

#endif
