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
#include <algorithm>
#include <cwctype>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <winrt/base.h>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.ApplicationModel.Core.h>
#include <winrt/Windows.Devices.Input.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Gaming.Input.h>
#include <winrt/Windows.Graphics.Display.h>
#include <winrt/Windows.Networking.h>
#include <winrt/Windows.Networking.Connectivity.h>
#include <winrt/Windows.Networking.Sockets.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Storage.AccessCache.h>
#include <winrt/Windows.Storage.Pickers.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.Input.h>
#include <winrt/Windows.UI.ViewManagement.h>

extern "C" {
void D3D11_DoResize(int newwidth, int newheight);
void D3D11_BeginSuspend(void);
void D3D11_EndSuspend(void);
}

using namespace winrt;
using namespace Windows::ApplicationModel;
using namespace Windows::ApplicationModel::Core;
using namespace Windows::Devices::Input;
using namespace Windows::Foundation;
using namespace Windows::Gaming::Input;
using namespace Windows::Graphics::Display;
using namespace Windows::Networking;
using namespace Windows::Networking::Connectivity;
using namespace Windows::Networking::Sockets;
using namespace Windows::Storage;
using namespace Windows::Storage::AccessCache;
using namespace Windows::Storage::Pickers;
using namespace Windows::Storage::Streams;
using namespace Windows::System;
using namespace Windows::UI::Core;
using namespace Windows::UI::Input;

typedef winrt::com_ptr<IUnknown> UnknownPtr;

typedef winrt::Windows::UI::Core::CoreWindow WinRTCoreWindow;

typedef winrt::Windows::UI::Core::WindowSizeChangedEventArgs WinRTWindowSizeChangedEventArgs;
typedef winrt::Windows::UI::Core::VisibilityChangedEventArgs WinRTVisibilityChangedEventArgs;
typedef winrt::Windows::UI::Core::WindowActivatedEventArgs WinRTWindowActivatedEventArgs;

qboolean isDedicated = false;

static std::once_flag g_runtimeInit;
static std::mutex g_logMutex;
static LARGE_INTEGER g_timeFreq = {};
static LARGE_INTEGER g_timeStart = {};
static std::atomic<bool> g_timeReady{false};
static std::atomic<int> g_cachedWidth{1280};
static std::atomic<int> g_cachedHeight{720};
static std::atomic<double> g_cachedScale{1.0};
static std::atomic<bool> g_windowVisible{true};
static std::atomic<bool> g_windowActivated{true};

static WinRTCoreWindow g_coreWindow{nullptr};
static UnknownPtr g_coreWindowUnknown;
static winrt::event_revoker<WinRTCoreWindow> g_sizeChangedRevoker;
static winrt::event_revoker<WinRTCoreWindow> g_visibilityChangedRevoker;
static winrt::event_revoker<WinRTCoreWindow> g_activatedRevoker;
static winrt::event_revoker<WinRTCoreWindow> g_keyDownRevoker;
static winrt::event_revoker<WinRTCoreWindow> g_keyUpRevoker;
static winrt::event_revoker<WinRTCoreWindow> g_characterRevoker;
static winrt::event_revoker<WinRTCoreWindow> g_pointerPressedRevoker;
static winrt::event_revoker<WinRTCoreWindow> g_pointerReleasedRevoker;
static winrt::event_revoker<WinRTCoreWindow> g_pointerMovedRevoker;
static winrt::event_revoker<WinRTCoreWindow> g_pointerWheelRevoker;
static DisplayInformation g_displayInformation{nullptr};
static winrt::event_revoker<DisplayInformation> g_dpiChangedRevoker;
static winrt::event_revoker<CoreApplication> g_suspendingRevoker;
static winrt::event_revoker<CoreApplication> g_resumingRevoker;
static winrt::event_revoker<Gamepad> g_gamepadAddedRevoker;
static winrt::event_revoker<Gamepad> g_gamepadRemovedRevoker;

static std::wstring g_localStatePath;
static std::mutex g_externalMapMutex;
static bool g_externalMapLoaded = false;
static std::unordered_map<std::wstring, std::wstring> g_externalFileMap;
static constexpr wchar_t kExternalFileMapSettingKey[] = L"WinRTExternalFileMap";

static char *g_clipboardText = nullptr;

static constexpr float kTriggerButtonThreshold = 0.5f;

enum class WinRTEventType
{
        Key,
        Character,
        Pointer,
};

struct WinRTQueuedEvent
{
        WinRTEventType type{WinRTEventType::Key};
        unsigned int device{0};
        bool pressed{false};
        int key{0};
        int unicode{0};
        bool absolute{true};
        float x{0.0f};
        float y{0.0f};
        float z{0.0f};
        float size{0.0f};
};

struct WinRTGamepadState
{
        Gamepad device{nullptr};
        unsigned int qdevid{DEVID_UNSET};
        GamepadReading lastReading{};
        GamepadButtons lastButtons{GamepadButtons::None};
        bool leftTriggerPressed{false};
        bool rightTriggerPressed{false};
};

static std::mutex g_inputMutex;
static std::deque<WinRTQueuedEvent> g_inputQueue;
static double g_pointerX{0.0};
static double g_pointerY{0.0};
static bool g_pointerValid{false};
static unsigned int g_keyboardDeviceId{0};
static unsigned int g_pointerDeviceId{0};

static std::mutex g_gamepadMutex;
static std::vector<WinRTGamepadState> g_gamepads;

static unsigned short scantokey[] =
{
//      0                       1                       2                       3                       4                       5                       6                       7
//      8                       9                       A                       B                       C                       D                       E                       F
        0,                      27,                     '1',            '2',            '3',            '4',            '5',            '6',            // 0
        '7',            '8',            '9',            '0',            '-',            '=',            K_BACKSPACE,    9,              // 0
        'q',            'w',            'e',            'r',            't',            'y',            'u',            'i',            // 1
        'o',            'p',            '[',            ']',            K_ENTER,        K_LCTRL,        'a',            's',            // 1
        'd',            'f',            'g',            'h',            'j',            'k',            'l',            ';',            // 2
        '\'',           '`',            K_LSHIFT,       '\\',           'z',            'x',            'c',            'v',            // 2
        'b',            'n',            'm',            ',',            '.',            '/',            K_RSHIFT,       K_KP_STAR,      // 3
        K_LALT,         ' ',            K_CAPSLOCK,     K_F1,           K_F2,           K_F3,           K_F4,           K_F5,           // 3
        K_F6,           K_F7,           K_F8,           K_F9,           K_F10,          K_PAUSE,        K_SCRLCK,       K_KP_HOME,      // 4
        K_KP_UPARROW,   K_KP_PGUP,      K_KP_MINUS,     K_KP_LEFTARROW, K_KP_5,        K_KP_RIGHTARROW,K_KP_PLUS,      K_KP_END,       // 4
        K_KP_DOWNARROW, K_KP_PGDN,      K_KP_INS,       K_KP_DEL,       0,              0,              0,              K_F11,          // 5
        K_F12,          0,              0,              0,              0,              0,              0,              0,              // 5
        0,              0,              0,              0,              0,              '\\',          0,              0,              // 6
        0,              0,              0,              0,              0,              0,              0,              0,              // 6
        0,              0,              0,              0,              0,              0,              0,              0,              // 7
        0,              0,              0,              0,              0,              0,              0,              0,              // 7
//      0                       1                       2                       3                       4                       5                       6                       7
//      8                       9                       A                       B                       C                       D                       E                       F
        0,              0,              0,              0,              0,              0,              0,              0,              // 8
        0,              0,              0,              0,              0,              0,              0,              0,              // 8
        0,              0,              0,              0,              0,              0,              0,              0,              // 9
        0,              0,              0,              0,              0,              0,              0,              0,              // 9
        0,              0,              0,              0,              0,              0,              0,              0,              // a
        0,              0,              0,              0,              0,              0,              0,              0,              // a
        0,              0,              0,              0,              0,              0,              0,              0,              // b
        0,              0,              0,              0,              0,              0,              0,              0,              // b
        0,              0,              0,              0,              0,              0,              0,              0,              // c
        0,              0,              0,              0,              0,              0,              0,              0,              // c
        0,              0,              0,              0,              0,              0,              0,              0,              // d
        0,              0,              0,              0,              0,              0,              0,              0,              // d
        0,              0,              0,              0,              0,              0,              0,              0,              // e
        0,              0,              0,              0,              0,              0,              0,              0,              // e
        0,              0,              0,              0,              0,              0,              0,              0,              // f
        0,              0,              0,              0,              0,              0,              0,              0,              // f
//      0                       1                       2                       3                       4                       5                       6                       7
//      8                       9                       A                       B                       C                       D                       E                       F
        0,                      27,                     '1',            '2',            '3',            '4',            '5',            '6',            // 0
        '7',            '8',            '9',            '0',            '-',            '=',            K_BACKSPACE,    9,              // 0
        'q',            'w',            'e',            'r',            't',            'y',            'u',            'i',            // 1
        'o',            'p',            '[',            ']',            K_KP_ENTER,     K_RCTRL,        'a',            's',            // 1
        'd',            'f',            'g',            'h',            'j',            'k',            'l',            ';',            // 2
        '\'',           '`',            K_SHIFT,        '\\',           'z',            'x',            'c',            'v',            // 2
        'b',            'n',            'm',            ',',            '.',            K_KP_SLASH,     K_SHIFT,        K_PRINTSCREEN,// 3
        K_RALT,         ' ',            K_CAPSLOCK,     K_F1,           K_F2,           K_F3,           K_F4,           K_F5,           // 3
        K_F6,           K_F7,           K_F8,           K_F9,           K_F10,          K_KP_NUMLOCK,   K_SCRLCK,       K_HOME,         // 4
        K_UPARROW,      K_PGUP,         '-',            K_LEFTARROW,    0,              K_RIGHTARROW,   '+',            K_END,          // 4
        K_DOWNARROW,    K_PGDN,         K_INS,          K_DEL,          0,              0,              0,              K_F11,          // 5
        K_F12,          0,              0,              0,              0,              0,              0,              0,              // 5
        0,              0,              0,              0,              0,              '\\',          0,              0,              // 6
        0,              0,              0,              0,              0,              0,              0,              0,              // 6
        0,              0,              0,              0,              0,              0,              0,              0,              // 7
        0,              0,              0,              0,              0,              0,              0,              0               // 7
//      0                       1                       2                       3                       4                       5                       6                       7
//      8                       9                       A                       B                       C                       D                       E                       F
};

static int SysWinRT_MapScanCode(uint32_t scancode)
{
        int key = 0;
        if (scancode < sizeof(scantokey) / sizeof(scantokey[0]))
                key = scantokey[scancode];
        if (!cl_keypad.ival)
        {
                switch (key)
                {
                case K_KP_HOME:         return '7';
                case K_KP_UPARROW:      return '8';
                case K_KP_PGUP:         return '9';
                case K_KP_LEFTARROW:    return '4';
                case K_KP_5:            return '5';
                case K_KP_RIGHTARROW:   return '6';
                case K_KP_END:          return '1';
                case K_KP_DOWNARROW:    return '2';
                case K_KP_PGDN:         return '3';
                case K_KP_ENTER:        return K_ENTER;
                case K_KP_INS:          return '0';
                case K_KP_DEL:          return '.';
                case K_KP_SLASH:        return '/';
                case K_KP_MINUS:        return '-';
                case K_KP_PLUS:         return '+';
                case K_KP_STAR:         return '*';
                }
        }
        if (!key)
                Con_DPrintf("key 0x%02x has no translation\n", scancode);
        return key;
}

static void SysWinRT_QueueEvent(const WinRTQueuedEvent &ev)
{
        std::lock_guard<std::mutex> guard(g_inputMutex);
        if (g_inputQueue.size() >= 2048)
                g_inputQueue.pop_front();
        g_inputQueue.push_back(ev);
}

static void SysWinRT_PostKeyEvent(unsigned int device, bool pressed, int key)
{
        if (!key)
                return;
        WinRTQueuedEvent ev;
        ev.type = WinRTEventType::Key;
        ev.device = device;
        ev.pressed = pressed;
        ev.key = key;
        SysWinRT_QueueEvent(ev);
}

static void SysWinRT_PostCharEvent(unsigned int device, int unicode)
{
        if (!unicode)
                return;
        WinRTQueuedEvent ev;
        ev.type = WinRTEventType::Character;
        ev.device = device;
        ev.unicode = unicode;
        SysWinRT_QueueEvent(ev);
}

static void SysWinRT_PostPointerEvent(unsigned int device, bool absolute, float x, float y, float z, float size)
{
        WinRTQueuedEvent ev;
        ev.type = WinRTEventType::Pointer;
        ev.device = device;
        ev.absolute = absolute;
        ev.x = x;
        ev.y = y;
        ev.z = z;
        ev.size = size;
        SysWinRT_QueueEvent(ev);
}

static float SysWinRT_ToPixels(double value)
{
        double scale = g_cachedScale.load();
        return static_cast<float>(value * scale);
}

static void SysWinRT_AddGamepad(Gamepad const &pad)
{
        if (!pad)
                return;
        WinRTGamepadState state;
        state.device = pad;
        state.qdevid = static_cast<unsigned int>(g_gamepads.size());
        state.lastReading = pad.GetCurrentReading();
        state.lastButtons = state.lastReading.Buttons;
        state.leftTriggerPressed = state.lastReading.LeftTrigger >= kTriggerButtonThreshold;
        state.rightTriggerPressed = state.lastReading.RightTrigger >= kTriggerButtonThreshold;
        g_gamepads.push_back(state);
}

static void SysWinRT_RemoveGamepad(Gamepad const &pad)
{
        if (!pad)
                return;
        g_gamepads.erase(std::remove_if(g_gamepads.begin(), g_gamepads.end(),
                [&](const WinRTGamepadState &state)
                {
                        return state.device == pad;
                }), g_gamepads.end());
}

static void SysWinRT_ProcessGamepads(void)
{
        std::lock_guard<std::mutex> guard(g_gamepadMutex);
        for (auto &state : g_gamepads)
        {
                        if (!state.device)
                                continue;
                        auto reading = state.device.GetCurrentReading();
                        GamepadButtons buttons = reading.Buttons;
                        GamepadButtons changed = buttons ^ state.lastButtons;
                        unsigned int devid = state.qdevid;
                        if (devid == DEVID_UNSET)
                        {
                                state.lastButtons = buttons;
                                state.lastReading = reading;
                                state.leftTriggerPressed = reading.LeftTrigger >= kTriggerButtonThreshold;
                                state.rightTriggerPressed = reading.RightTrigger >= kTriggerButtonThreshold;
                                continue;
                        }

                        auto sendButton = [&](GamepadButtons flag, int key)
                        {
                                if (static_cast<bool>(changed & flag))
                                        IN_KeyEvent(devid, static_cast<bool>(buttons & flag), key, 0);
                        };

                        sendButton(GamepadButtons::A, K_GP_DIAMOND_DOWN);
                        sendButton(GamepadButtons::B, K_GP_DIAMOND_RIGHT);
                        sendButton(GamepadButtons::X, K_GP_DIAMOND_LEFT);
                        sendButton(GamepadButtons::Y, K_GP_DIAMOND_UP);
                        sendButton(GamepadButtons::View, K_GP_VIEW);
                        sendButton(GamepadButtons::Menu, K_GP_MENU);
                        sendButton(GamepadButtons::LeftShoulder, K_GP_LEFT_SHOULDER);
                        sendButton(GamepadButtons::RightShoulder, K_GP_RIGHT_SHOULDER);
                        sendButton(GamepadButtons::LeftThumbstick, K_GP_LEFT_STICK);
                        sendButton(GamepadButtons::RightThumbstick, K_GP_RIGHT_STICK);
                        sendButton(GamepadButtons::DPadUp, K_GP_DPAD_UP);
                        sendButton(GamepadButtons::DPadDown, K_GP_DPAD_DOWN);
                        sendButton(GamepadButtons::DPadLeft, K_GP_DPAD_LEFT);
                        sendButton(GamepadButtons::DPadRight, K_GP_DPAD_RIGHT);
                        sendButton(GamepadButtons::Guide, K_GP_GUIDE);
                        sendButton(GamepadButtons::Paddle1, K_GP_PADDLE1);
                        sendButton(GamepadButtons::Paddle2, K_GP_PADDLE2);
                        sendButton(GamepadButtons::Paddle3, K_GP_PADDLE3);
                        sendButton(GamepadButtons::Paddle4, K_GP_PADDLE4);
                        sendButton(GamepadButtons::Misc1, K_GP_MISC1);

                        bool leftTriggerNow = reading.LeftTrigger >= kTriggerButtonThreshold;
                        bool rightTriggerNow = reading.RightTrigger >= kTriggerButtonThreshold;
                        if (leftTriggerNow != state.leftTriggerPressed)
                                IN_KeyEvent(devid, leftTriggerNow, K_GP_LEFT_TRIGGER, 0);
                        if (rightTriggerNow != state.rightTriggerPressed)
                                IN_KeyEvent(devid, rightTriggerNow, K_GP_RIGHT_TRIGGER, 0);

                        state.leftTriggerPressed = leftTriggerNow;
                        state.rightTriggerPressed = rightTriggerNow;

                        IN_JoystickAxisEvent(devid, GPAXIS_LT_RIGHT, static_cast<float>(reading.LeftThumbstickX));
                        IN_JoystickAxisEvent(devid, GPAXIS_LT_DOWN, static_cast<float>(-reading.LeftThumbstickY));
                        IN_JoystickAxisEvent(devid, GPAXIS_RT_RIGHT, static_cast<float>(reading.RightThumbstickX));
                        IN_JoystickAxisEvent(devid, GPAXIS_RT_DOWN, static_cast<float>(-reading.RightThumbstickY));
                        IN_JoystickAxisEvent(devid, GPAXIS_LT_TRIGGER, static_cast<float>(reading.LeftTrigger));
                        IN_JoystickAxisEvent(devid, GPAXIS_RT_TRIGGER, static_cast<float>(reading.RightTrigger));

                        state.lastButtons = buttons;
                        state.lastReading = reading;
        }
}

static bool SysWinRT_SplitRelative(const std::wstring &resolved, std::vector<std::wstring> &segments)
{
        segments.clear();
        if (g_localStatePath.empty())
                return false;

        std::wstring base = g_localStatePath;
        if (!base.empty() && base.back() != L'\\')
                base.push_back(L'\\');
        if (resolved.size() < base.size())
                return false;
        if (CompareStringOrdinal(resolved.c_str(), static_cast<int>(base.size()), base.c_str(), static_cast<int>(base.size()), TRUE) != CSTR_EQUAL)
                return false;

        std::wstring relative = resolved.substr(base.size());
        size_t start = 0;
        while (start < relative.size())
        {
                size_t pos = relative.find(L'\\', start);
                std::wstring part = relative.substr(start, pos == std::wstring::npos ? std::wstring::npos : pos - start);
                if (!part.empty())
                        segments.push_back(part);
                if (pos == std::wstring::npos)
                        break;
                start = pos + 1;
        }
        return true;
}

static void SysWinRT_OpenFilePicker_f(void)
{
        try
        {
                FileOpenPicker picker;
                picker.ViewMode(PickerViewMode::List);
                picker.SuggestedStartLocation(PickerLocationId::DocumentsLibrary);
                picker.FileTypeFilter().Clear();
                picker.FileTypeFilter().Append(L"*");

                auto file = picker.PickSingleFileAsync().get();
                if (!file)
                        return;
                auto path = file.Path();
                if (path.empty())
                        return;
                char utf8[MAX_OSPATH];
                if (WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, utf8, sizeof(utf8), NULL, NULL) <= 0)
                        return;
                vfsfile_t *handle = VFSOS_Open(utf8, "rb");
                if (handle)
                {
                        const char *display = COM_SkipPath(utf8);
                        Host_RunFile(display, strlen(display), handle);
                }
        }
        catch (const winrt::hresult_error &)
        {
        }
}

static WinRTCoreWindow SysWinRT_TryGetWindow();
static void SysWinRT_HandleSuspended();
static void SysWinRT_HandleResumed();

static void SysWinRT_UpdateActivity(bool visible, bool activated)
{
        static bool soundActive = false;

        bool oldActive = vid.activeapp;
        bool oldMinimized = vid.isminimized;
        bool newActive = visible && activated;

        vid.activeapp = newActive;
        vid.isminimized = !visible;

        if (!vid.activeapp && soundActive)
        {
                S_BlockSound();
                soundActive = false;
        }
        else if (vid.activeapp && !soundActive)
        {
                S_UnblockSound();
                soundActive = true;
        }

        if (oldActive != vid.activeapp || oldMinimized != vid.isminimized)
                INS_UpdateGrabs(vid.fullscreen, vid.activeapp);
}

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

static std::string SysWinRT_ToUtf8(const std::wstring &wide)
{
        if (wide.empty())
                return std::string();
        int required = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
        if (required <= 0)
                return std::string();
        std::string utf8(static_cast<size_t>(required), '\0');
        WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), utf8.data(), required, nullptr, nullptr);
        return utf8;
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

static void SysWinRT_HandleSuspended()
{
        Sys_Printf("WinRT suspending\n");
        SysWinRT_UpdateActivity(false, false);
        D3D11_BeginSuspend();
}

static void SysWinRT_HandleResumed()
{
        Sys_Printf("WinRT resumed\n");
        auto window = SysWinRT_TryGetWindow();
        if (window)
        {
                auto bounds = window.Bounds();
                SysWinRT_UpdateCachedSize(bounds.Width, bounds.Height);
                g_windowVisible = window.Visible();
        }
        g_windowActivated = true;
        D3D11_EndSuspend();
        SysWinRT_UpdateActivity(g_windowVisible.load(), g_windowActivated.load());
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

        g_windowVisible = window.Visible();
        g_windowActivated = true;
        SysWinRT_UpdateActivity(g_windowVisible.load(), g_windowActivated.load());

        g_visibilityChangedRevoker = window.VisibilityChanged(winrt::auto_revoke,
                [](WinRTCoreWindow const &, WinRTVisibilityChangedEventArgs const &args)
                {
                        g_windowVisible = args.Visible();
                        SysWinRT_UpdateActivity(g_windowVisible.load(), g_windowActivated.load());
                });

        g_activatedRevoker = window.Activated(winrt::auto_revoke,
                [](WinRTCoreWindow const &, WinRTWindowActivatedEventArgs const &args)
                {
                        g_windowActivated = args.WindowActivationState() != CoreWindowActivationState::Deactivated;
                        SysWinRT_UpdateActivity(g_windowVisible.load(), g_windowActivated.load());
                });

        g_keyDownRevoker = window.KeyDown(winrt::auto_revoke,
                [](WinRTCoreWindow const &, KeyEventArgs const &args)
                {
                        auto status = args.KeyStatus();
                        int key = SysWinRT_MapScanCode(status.ScanCode);
                        SysWinRT_PostKeyEvent(g_keyboardDeviceId, true, key);
                });

        g_keyUpRevoker = window.KeyUp(winrt::auto_revoke,
                [](WinRTCoreWindow const &, KeyEventArgs const &args)
                {
                        auto status = args.KeyStatus();
                        int key = SysWinRT_MapScanCode(status.ScanCode);
                        SysWinRT_PostKeyEvent(g_keyboardDeviceId, false, key);
                });

        g_characterRevoker = window.CharacterReceived(winrt::auto_revoke,
                [](WinRTCoreWindow const &, CharacterReceivedEventArgs const &args)
                {
                        SysWinRT_PostCharEvent(g_keyboardDeviceId, static_cast<int>(args.KeyCode()));
                });

        g_pointerMovedRevoker = window.PointerMoved(winrt::auto_revoke,
                [](WinRTCoreWindow const &, PointerEventArgs const &args)
                {
                        auto point = args.CurrentPoint();
                        g_pointerX = point.Position().X;
                        g_pointerY = point.Position().Y;
                        g_pointerValid = true;
                        SysWinRT_PostPointerEvent(g_pointerDeviceId, true,
                                SysWinRT_ToPixels(g_pointerX),
                                SysWinRT_ToPixels(g_pointerY),
                                0.0f, 0.0f);
                });

        g_pointerPressedRevoker = window.PointerPressed(winrt::auto_revoke,
                [](WinRTCoreWindow const &, PointerEventArgs const &args)
                {
                        auto point = args.CurrentPoint();
                        g_pointerX = point.Position().X;
                        g_pointerY = point.Position().Y;
                        g_pointerValid = true;
                        SysWinRT_PostPointerEvent(g_pointerDeviceId, true,
                                SysWinRT_ToPixels(g_pointerX),
                                SysWinRT_ToPixels(g_pointerY),
                                0.0f, 0.0f);

                        switch (point.Properties().PointerUpdateKind())
                        {
                        case PointerUpdateKind::LeftButtonPressed:
                                SysWinRT_PostKeyEvent(g_pointerDeviceId, true, K_MOUSE1);
                                break;
                        case PointerUpdateKind::RightButtonPressed:
                                SysWinRT_PostKeyEvent(g_pointerDeviceId, true, K_MOUSE2);
                                break;
                        case PointerUpdateKind::MiddleButtonPressed:
                                SysWinRT_PostKeyEvent(g_pointerDeviceId, true, K_MOUSE3);
                                break;
                        case PointerUpdateKind::XButton1Pressed:
                                SysWinRT_PostKeyEvent(g_pointerDeviceId, true, K_MOUSE4);
                                break;
                        case PointerUpdateKind::XButton2Pressed:
                                SysWinRT_PostKeyEvent(g_pointerDeviceId, true, K_MOUSE5);
                                break;
                        default:
                                break;
                        }
                });

        g_pointerReleasedRevoker = window.PointerReleased(winrt::auto_revoke,
                [](WinRTCoreWindow const &, PointerEventArgs const &args)
                {
                        auto point = args.CurrentPoint();
                        g_pointerX = point.Position().X;
                        g_pointerY = point.Position().Y;
                        g_pointerValid = true;
                        SysWinRT_PostPointerEvent(g_pointerDeviceId, true,
                                SysWinRT_ToPixels(g_pointerX),
                                SysWinRT_ToPixels(g_pointerY),
                                0.0f, 0.0f);

                        switch (point.Properties().PointerUpdateKind())
                        {
                        case PointerUpdateKind::LeftButtonReleased:
                                SysWinRT_PostKeyEvent(g_pointerDeviceId, false, K_MOUSE1);
                                break;
                        case PointerUpdateKind::RightButtonReleased:
                                SysWinRT_PostKeyEvent(g_pointerDeviceId, false, K_MOUSE2);
                                break;
                        case PointerUpdateKind::MiddleButtonReleased:
                                SysWinRT_PostKeyEvent(g_pointerDeviceId, false, K_MOUSE3);
                                break;
                        case PointerUpdateKind::XButton1Released:
                                SysWinRT_PostKeyEvent(g_pointerDeviceId, false, K_MOUSE4);
                                break;
                        case PointerUpdateKind::XButton2Released:
                                SysWinRT_PostKeyEvent(g_pointerDeviceId, false, K_MOUSE5);
                                break;
                        default:
                                break;
                        }
                });

        g_pointerWheelRevoker = window.PointerWheelChanged(winrt::auto_revoke,
                [](WinRTCoreWindow const &, PointerEventArgs const &args)
                {
                        auto point = args.CurrentPoint();
                        g_pointerX = point.Position().X;
                        g_pointerY = point.Position().Y;
                        g_pointerValid = true;
                        float delta = static_cast<float>(point.Properties().MouseWheelDelta()) / 120.0f;
                        SysWinRT_PostPointerEvent(g_pointerDeviceId, true,
                                SysWinRT_ToPixels(g_pointerX),
                                SysWinRT_ToPixels(g_pointerY),
                                delta, 0.0f);
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

        try
        {
                g_suspendingRevoker = CoreApplication::Suspending(winrt::auto_revoke,
                        [](IInspectable const &, SuspendingEventArgs const &args)
                        {
                                SuspendingOperation operation = args.SuspendingOperation();
                                SuspendingDeferral deferral = nullptr;
                                if (operation)
                                        deferral = operation.GetDeferral();
                                SysWinRT_HandleSuspended();
                                if (deferral)
                                        deferral.Complete();
                        });
                g_resumingRevoker = CoreApplication::Resuming(winrt::auto_revoke,
                        [](IInspectable const &, IInspectable const &)
                        {
                                SysWinRT_HandleResumed();
                        });
        }
        catch (const winrt::hresult_error &)
        {
        }

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
        Cmd_AddCommandD("sys_openfile", SysWinRT_OpenFilePicker_f, "Select a file to open/install/etc.");
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
        if (g_visibilityChangedRevoker)
        {
                try
                {
                        g_visibilityChangedRevoker.revoke();
                }
                catch (const winrt::hresult_error &)
                {
                }
        }
        if (g_activatedRevoker)
        {
                try
                {
                        g_activatedRevoker.revoke();
                }
                catch (const winrt::hresult_error &)
                {
                }
        }
        if (g_keyDownRevoker)
        {
                try
                {
                        g_keyDownRevoker.revoke();
                }
                catch (const winrt::hresult_error &)
                {
                }
        }
        if (g_keyUpRevoker)
        {
                try
                {
                        g_keyUpRevoker.revoke();
                }
                catch (const winrt::hresult_error &)
                {
                }
        }
        if (g_characterRevoker)
        {
                try
                {
                        g_characterRevoker.revoke();
                }
                catch (const winrt::hresult_error &)
                {
                }
        }
        if (g_pointerMovedRevoker)
        {
                try
                {
                        g_pointerMovedRevoker.revoke();
                }
                catch (const winrt::hresult_error &)
                {
                }
        }
        if (g_pointerPressedRevoker)
        {
                try
                {
                        g_pointerPressedRevoker.revoke();
                }
                catch (const winrt::hresult_error &)
                {
                }
        }
        if (g_pointerReleasedRevoker)
        {
                try
                {
                        g_pointerReleasedRevoker.revoke();
                }
                catch (const winrt::hresult_error &)
                {
                }
        }
        if (g_pointerWheelRevoker)
        {
                try
                {
                        g_pointerWheelRevoker.revoke();
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
        if (g_suspendingRevoker)
        {
                try
                {
                        g_suspendingRevoker.revoke();
                }
                catch (const winrt::hresult_error &)
                {
                }
        }
        if (g_resumingRevoker)
        {
                try
                {
                        g_resumingRevoker.revoke();
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
        std::deque<WinRTQueuedEvent> pending;
        {
                std::lock_guard<std::mutex> guard(g_inputMutex);
                pending.swap(g_inputQueue);
        }

        for (const auto &ev : pending)
        {
                switch (ev.type)
                {
                case WinRTEventType::Key:
                        IN_KeyEvent(ev.device, ev.pressed, ev.key, 0);
                        break;
                case WinRTEventType::Character:
                        IN_KeyEvent(ev.device, true, 0, ev.unicode);
                        IN_KeyEvent(ev.device, false, 0, ev.unicode);
                        break;
                case WinRTEventType::Pointer:
                        IN_MouseMove(ev.device, ev.absolute, ev.x, ev.y, ev.z, ev.size);
                        break;
                }
        }

        SysWinRT_ProcessGamepads();
}

void INS_Init(void)
{
        {
                std::lock_guard<std::mutex> guard(g_inputMutex);
                g_inputQueue.clear();
                g_pointerValid = false;
                g_keyboardDeviceId = 0;
                g_pointerDeviceId = 0;
        }

        {
                std::lock_guard<std::mutex> guard(g_gamepadMutex);
                g_gamepads.clear();
                try
                {
                        auto pads = Gamepad::Gamepads();
                        for (auto const &pad : pads)
                                SysWinRT_AddGamepad(pad);
                }
                catch (const winrt::hresult_error &)
                {
                }
        }

        try
        {
                g_gamepadAddedRevoker = Gamepad::GamepadAdded(winrt::auto_revoke,
                        [](IInspectable const &, Gamepad const &pad)
                        {
                                std::lock_guard<std::mutex> guard(g_gamepadMutex);
                                SysWinRT_AddGamepad(pad);
                        });
        }
        catch (const winrt::hresult_error &)
        {
        }

        try
        {
                g_gamepadRemovedRevoker = Gamepad::GamepadRemoved(winrt::auto_revoke,
                        [](IInspectable const &, Gamepad const &pad)
                        {
                                std::lock_guard<std::mutex> guard(g_gamepadMutex);
                                SysWinRT_RemoveGamepad(pad);
                        });
        }
        catch (const winrt::hresult_error &)
        {
        }
}

void INS_ReInit(void)
{
        INS_Init();
}

void INS_Shutdown(void)
{
        try
        {
                g_gamepadAddedRevoker.revoke();
        }
        catch (const winrt::hresult_error &)
        {
        }
        try
        {
                g_gamepadRemovedRevoker.revoke();
        }
        catch (const winrt::hresult_error &)
        {
        }
        std::lock_guard<std::mutex> guard(g_gamepadMutex);
        g_gamepads.clear();
}

void INS_ClearStates(void)
{
        {
                std::lock_guard<std::mutex> guard(g_inputMutex);
                g_inputQueue.clear();
        }
        {
                std::lock_guard<std::mutex> guard(g_gamepadMutex);
                for (auto &state : g_gamepads)
                {
                        state.lastButtons = GamepadButtons::None;
                        state.leftTriggerPressed = false;
                        state.rightTriggerPressed = false;
                        state.lastReading = state.device ? state.device.GetCurrentReading() : GamepadReading{};
                }
        }
}

void INS_UpdateGrabs(int fullscreen, int activeapp)
{
        (void)fullscreen;
        (void)activeapp;
}

void INS_SetupControllerAudioDevices(qboolean enabled)
{
        (void)enabled;
}

enum controllertype_e INS_GetControllerType(int id)
{
        std::lock_guard<std::mutex> guard(g_gamepadMutex);
        for (const auto &state : g_gamepads)
        {
                if (static_cast<int>(state.qdevid) == id)
                        return CONTROLLER_XBOX;
        }
        return CONTROLLER_UNKNOWN;
}

void INS_Rumble(int joy, quint16_t amp_low, quint16_t amp_high, quint32_t duration)
{
        (void)duration;
        std::lock_guard<std::mutex> guard(g_gamepadMutex);
        for (auto &state : g_gamepads)
        {
                if (state.qdevid != static_cast<unsigned int>(joy) || !state.device)
                        continue;
                GamepadVibration vib = state.device.Vibration();
                vib.LeftMotor = amp_low / 65535.0f;
                vib.RightMotor = amp_high / 65535.0f;
                state.device.SetVibration(vib);
                break;
        }
}

void INS_RumbleTriggers(int joy, quint16_t left, quint16_t right, quint32_t duration)
{
        (void)duration;
        std::lock_guard<std::mutex> guard(g_gamepadMutex);
        for (auto &state : g_gamepads)
        {
                if (state.qdevid != static_cast<unsigned int>(joy) || !state.device)
                        continue;
                GamepadVibration vib = state.device.Vibration();
                vib.LeftTrigger = left / 65535.0f;
                vib.RightTrigger = right / 65535.0f;
                state.device.SetVibration(vib);
                break;
        }
}

void INS_SetLEDColor(int id, vec3_t color)
{
        (void)id;
        (void)color;
}

void INS_SetTriggerFX(int id, const void *data, size_t size)
{
        (void)id;
        (void)data;
        (void)size;
}

qboolean INS_KeyToLocalName(int qkey, char *buf, size_t bufsize)
{
        (void)qkey;
        if (buf && bufsize)
                *buf = '\0';
        return false;
}

void INS_EnumerateDevices(void *ctx, void(*callback)(void *ctx, const char *type, const char *devicename, unsigned int *qdevid))
{
        if (!callback)
                return;

        callback(ctx, "keyboard", "CoreWindow Keyboard", &g_keyboardDeviceId);
        callback(ctx, "mouse", "CoreWindow Pointer", &g_pointerDeviceId);

        std::lock_guard<std::mutex> guard(g_gamepadMutex);
        size_t idx = 0;
        for (auto &state : g_gamepads)
        {
                char name[64];
                Q_snprintfz(name, sizeof(name), "Gamepad %zu", idx++);
                callback(ctx, "joystick", name, &state.qdevid);
        }
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

struct WinRTFileMode
{
        bool read{false};
        bool write{false};
        bool append{false};
        bool create{false};
        bool truncate{false};
};

static WinRTFileMode SysWinRT_ParseFileMode(const char *mode)
{
        WinRTFileMode fm{};
        if (!mode || !*mode)
        {
                fm.read = true;
                return fm;
        }
        char primary = mode[0];
        switch (primary)
        {
        case 'w':
        case 'W':
                fm.write = true;
                fm.create = true;
                fm.truncate = true;
                break;
        case 'a':
        case 'A':
                fm.write = true;
                fm.create = true;
                fm.append = true;
                break;
        default:
                fm.read = true;
                break;
        }
        for (const char *p = mode + 1; *p; ++p)
        {
                switch (*p)
                {
                case '+':
                        fm.read = true;
                        fm.write = true;
                        break;
                case 'r':
                case 'R':
                        fm.read = true;
                        break;
                case 'w':
                case 'W':
                        fm.write = true;
                        fm.create = true;
                        fm.truncate = true;
                        break;
                case 'a':
                case 'A':
                        fm.write = true;
                        fm.create = true;
                        fm.append = true;
                        break;
                default:
                        break;
                }
        }
        if (!fm.read && !fm.write)
                fm.read = true;
        return fm;
}

static std::wstring SysWinRT_NormalizeKey(const std::wstring &path)
{
        std::wstring key = path;
        SysWinRT_NormalizeSlashes(key);
        for (auto &ch : key)
                ch = static_cast<wchar_t>(std::towlower(ch));
        return key;
}

static void SysWinRT_LoadExternalFileMap()
{
        std::scoped_lock lock(g_externalMapMutex);
        if (g_externalMapLoaded)
                return;
        g_externalMapLoaded = true;
        try
        {
                auto settings = ApplicationData::Current().LocalSettings();
                if (!settings)
                        return;
                auto values = settings.Values();
                if (!values)
                        return;
                auto boxed = values.Lookup(kExternalFileMapSettingKey);
                if (!boxed)
                        return;
                std::wstring data = winrt::unbox_value_or<hstring>(boxed, L"");
                size_t start = 0;
                while (start < data.size())
                {
                        size_t end = data.find(L'\n', start);
                        std::wstring line = data.substr(start, (end == std::wstring::npos) ? std::wstring::npos : end - start);
                        if (!line.empty())
                        {
                                size_t sep = line.find(L'|');
                                if (sep != std::wstring::npos)
                                {
                                        std::wstring key = line.substr(0, sep);
                                        std::wstring value = line.substr(sep + 1);
                                        if (!key.empty() && !value.empty())
                                                g_externalFileMap.emplace(std::move(key), std::move(value));
                                }
                        }
                        if (end == std::wstring::npos)
                                break;
                        start = end + 1;
                }
        }
        catch (const winrt::hresult_error &)
        {
                g_externalFileMap.clear();
        }
}

static void SysWinRT_SaveExternalFileMap()
{
        std::scoped_lock lock(g_externalMapMutex);
        if (!g_externalMapLoaded)
                return;
        try
        {
                std::wstring packed;
                bool first = true;
                for (const auto &entry : g_externalFileMap)
                {
                        if (!first)
                                packed.push_back(L'\n');
                        first = false;
                        packed.append(entry.first);
                        packed.push_back(L'|');
                        packed.append(entry.second);
                }
                auto settings = ApplicationData::Current().LocalSettings();
                if (settings)
                {
                        auto values = settings.Values();
                        if (values)
                                values.Insert(kExternalFileMapSettingKey, box_value(hstring(packed)));
                }
        }
        catch (const winrt::hresult_error &)
        {
        }
}

static std::wstring SysWinRT_LookupExternalMapping(const std::wstring &path)
{
        SysWinRT_LoadExternalFileMap();
        std::scoped_lock lock(g_externalMapMutex);
        auto key = SysWinRT_NormalizeKey(path);
        auto it = g_externalFileMap.find(key);
        if (it != g_externalFileMap.end())
                return it->second;
        return std::wstring();
}

static void SysWinRT_StoreExternalMapping(const std::wstring &path, const std::wstring &relative)
{
        SysWinRT_LoadExternalFileMap();
        std::scoped_lock lock(g_externalMapMutex);
        g_externalFileMap[SysWinRT_NormalizeKey(path)] = relative;
        SysWinRT_SaveExternalFileMap();
}

static std::wstring SysWinRT_SanitizeSegment(std::wstring segment)
{
        for (auto &ch : segment)
        {
                if (ch < 32 || ch == L'<') ch = L'_';
                else if (ch == L'>') ch = L'_';
                else if (ch == L':') ch = L'_';
                else if (ch == L'"') ch = L'_';
                else if (ch == L'/') ch = L'_';
                else if (ch == L'\\') ch = L'_';
                else if (ch == L'|') ch = L'_';
                else if (ch == L'?') ch = L'_';
                else if (ch == L'*') ch = L'_';
        }
        while (!segment.empty() && (segment.back() == L'.' || segment.back() == L' '))
                segment.pop_back();
        if (segment.empty())
                segment = L"file";
        return segment;
}

static std::wstring SysWinRT_HexFromHash(size_t value)
{
        wchar_t buffer[17];
        swprintf(buffer, 17, L"%016llx", static_cast<unsigned long long>(value));
        return buffer;
}

static std::wstring SysWinRT_GenerateImportedRelative(const std::wstring &path, const std::wstring &fileName)
{
        std::wstring sanitized = SysWinRT_SanitizeSegment(fileName);
        size_t dot = sanitized.find_last_of(L'.');
        std::wstring base = sanitized;
        std::wstring extension;
        if (dot != std::wstring::npos)
        {
                base = sanitized.substr(0, dot);
                extension = sanitized.substr(dot);
        }
        size_t hash = std::hash<std::wstring>{}(SysWinRT_NormalizeKey(path));
        std::wstring relative = L"ImportedContent\\";
        relative.append(base);
        relative.push_back(L'_');
        relative.append(SysWinRT_HexFromHash(hash));
        relative.append(extension);
        return relative;
}

static std::optional<std::wstring> SysWinRT_RequestExternalImport(const std::wstring &path, const WinRTFileMode &mode)
{
        if (mode.write)
        {
                Sys_Printf("%sWinRT sandbox blocks writing to external paths.\n", CON_ERROR);
                return std::nullopt;
        }
        if (g_localStatePath.empty())
                return std::nullopt;
        try
        {
                FileOpenPicker picker;
                picker.ViewMode(PickerViewMode::List);
                picker.SuggestedStartLocation(PickerLocationId::DocumentsLibrary);
                picker.FileTypeFilter().Clear();
                picker.FileTypeFilter().Append(L"*");
                Sys_Printf("%sSelect '%s' for import.\n", CON_INFO, SysWinRT_ToUtf8(path).c_str());
                StorageFile file = picker.PickSingleFileAsync().get();
                if (!file)
                        return std::nullopt;
                auto localFolder = ApplicationData::Current().LocalFolder();
                if (!localFolder)
                        return std::nullopt;
                StorageFolder importFolder = localFolder.CreateFolderAsync(L"ImportedContent", CreationCollisionOption::OpenIfExists).get();
                std::wstring relative = SysWinRT_GenerateImportedRelative(path, file.Name().c_str());
                std::vector<std::wstring> segments;
                if (!SysWinRT_SplitRelative(SysWinRT_Combine(g_localStatePath, relative), segments))
                        return std::nullopt;
                StorageFolder target = importFolder;
                if (segments.size() > 1)
                {
                        for (size_t i = 1; i + 1 < segments.size(); ++i)
                                target = target.CreateFolderAsync(segments[i], CreationCollisionOption::OpenIfExists).get();
                }
                std::wstring leaf = segments.empty() ? std::wstring() : segments.back();
                if (leaf.empty())
                        leaf = SysWinRT_SanitizeSegment(file.Name().c_str());
                file.CopyAsync(target, leaf, NameCollisionOption::ReplaceExisting).get();
                SysWinRT_StoreExternalMapping(path, relative);
                Sys_Printf("%sImported '%s' into sandbox as '%s'.\n", CON_INFO, SysWinRT_ToUtf8(path).c_str(), SysWinRT_ToUtf8(relative).c_str());
                return relative;
        }
        catch (const winrt::hresult_error &)
        {
        }
        return std::nullopt;
}

struct WinRTFile : vfsfile_s
{
        IRandomAccessStream stream{nullptr};
        WinRTFileMode mode{};
        bool dirty{false};
};

static int QDECL VFSWinRT_ReadBytes(struct vfsfile_s *file, void *buffer, int bytestoread)
{
        auto *handle = static_cast<WinRTFile *>(file);
        if (!handle || !handle->stream || !handle->mode.read || bytestoread <= 0)
                return 0;
        try
        {
                DataReader reader(handle->stream);
                reader.InputStreamOptions(InputStreamOptions::Partial);
                uint32_t toRead = static_cast<uint32_t>(bytestoread);
                uint32_t loaded = reader.LoadAsync(toRead).get();
                if (!loaded)
                {
                        reader.DetachStream();
                        return 0;
                }
                reader.ReadBytes({reinterpret_cast<uint8_t *>(buffer), reinterpret_cast<uint8_t *>(buffer) + loaded});
                reader.DetachStream();
                return static_cast<int>(loaded);
        }
        catch (const winrt::hresult_error &)
        {
                return 0;
        }
}

static int QDECL VFSWinRT_WriteBytes(struct vfsfile_s *file, const void *buffer, int bytestowrite)
{
        auto *handle = static_cast<WinRTFile *>(file);
        if (!handle || !handle->stream || !handle->mode.write || bytestowrite <= 0)
                return 0;
        try
        {
                DataWriter writer(handle->stream);
                writer.WriteBytes({reinterpret_cast<const uint8_t *>(buffer), reinterpret_cast<const uint8_t *>(buffer) + bytestowrite});
                writer.StoreAsync().get();
                writer.DetachStream();
                handle->dirty = true;
                return bytestowrite;
        }
        catch (const winrt::hresult_error &)
        {
                return 0;
        }
}

static qboolean QDECL VFSWinRT_Seek(struct vfsfile_s *file, qofs_t pos)
{
        auto *handle = static_cast<WinRTFile *>(file);
        if (!handle || !handle->stream)
                return false;
        if (pos < 0)
                return false;
        try
        {
                handle->stream.Seek(static_cast<uint64_t>(pos));
                return true;
        }
        catch (const winrt::hresult_error &)
        {
                return false;
        }
}

static qofs_t QDECL VFSWinRT_Tell(struct vfsfile_s *file)
{
        auto *handle = static_cast<WinRTFile *>(file);
        if (!handle || !handle->stream)
                return 0;
        try
        {
                return static_cast<qofs_t>(handle->stream.Position());
        }
        catch (const winrt::hresult_error &)
        {
                return 0;
        }
}

static qofs_t QDECL VFSWinRT_GetLen(struct vfsfile_s *file)
{
        auto *handle = static_cast<WinRTFile *>(file);
        if (!handle || !handle->stream)
                return 0;
        try
        {
                return static_cast<qofs_t>(handle->stream.Size());
        }
        catch (const winrt::hresult_error &)
        {
                return 0;
        }
}

static void QDECL VFSWinRT_Flush(struct vfsfile_s *file)
{
        auto *handle = static_cast<WinRTFile *>(file);
        if (!handle || !handle->stream || !handle->dirty)
                return;
        try
        {
                handle->stream.FlushAsync().get();
                handle->dirty = false;
        }
        catch (const winrt::hresult_error &)
        {
        }
}

static qboolean QDECL VFSWinRT_Close(struct vfsfile_s *file)
{
        auto *handle = static_cast<WinRTFile *>(file);
        if (!handle)
                return false;
        VFSWinRT_Flush(file);
        try
        {
                if (handle->stream)
                        handle->stream.Close();
        }
        catch (const winrt::hresult_error &)
        {
        }
        Z_Free(handle);
        return true;
}

extern "C" vfsfile_t *VFSWinRT_Open(const char *osname, const char *mode)
{
        WinRTFileMode parsed = SysWinRT_ParseFileMode(mode);
        if (fs_readonly && parsed.write)
                return NULL;
        std::wstring resolved = SysWinRT_ResolvePath(osname);
        if (resolved.empty())
                return NULL;
        std::vector<std::wstring> segments;
        bool insideSandbox = SysWinRT_SplitRelative(resolved, segments);
        if (!insideSandbox)
        {
                std::wstring mapping = SysWinRT_LookupExternalMapping(resolved);
                if (mapping.empty())
                {
                        auto imported = SysWinRT_RequestExternalImport(resolved, parsed);
                        if (!imported)
                        {
                                Sys_Printf("%sAccess to '%s' denied by sandbox.\n", CON_WARNING, osname ? osname : "");
                                return NULL;
                        }
                        mapping = *imported;
                }
                resolved = SysWinRT_Combine(g_localStatePath, mapping);
                if (!SysWinRT_SplitRelative(resolved, segments))
                        return NULL;
        }
        if (!g_localStatePath.size())
                return NULL;
        if (segments.empty())
                return NULL;
        try
        {
                StorageFolder folder = ApplicationData::Current().LocalFolder();
                for (size_t i = 0; i + 1 < segments.size(); ++i)
                {
                        folder = folder.CreateFolderAsync(segments[i], CreationCollisionOption::OpenIfExists).get();
                }
                std::wstring leaf = segments.back();
                StorageFile file = nullptr;
                if (parsed.create)
                {
                        CreationCollisionOption option = parsed.truncate ? CreationCollisionOption::ReplaceExisting : CreationCollisionOption::OpenIfExists;
                        file = folder.CreateFileAsync(leaf, option).get();
                }
                else
                {
                        file = folder.GetFileAsync(leaf).get();
                }
                if (!file)
                        return NULL;
                FileAccessMode fam = parsed.write ? FileAccessMode::ReadWrite : FileAccessMode::Read;
                IRandomAccessStream stream = file.OpenAsync(fam).get();
                if (!stream)
                        return NULL;
                if (parsed.truncate && parsed.write)
                        stream.Size(0);
                if (parsed.append)
                        stream.Seek(stream.Size());
                WinRTFile *handle = (WinRTFile *)Z_Malloc(sizeof(WinRTFile));
                memset(handle, 0, sizeof(WinRTFile));
                handle->stream = stream;
                handle->mode = parsed;
                handle->dirty = false;
#ifdef _DEBUG
                Q_strncpyz(handle->dbgname, osname ? osname : "VFSWinRT", sizeof(handle->dbgname));
#endif
                handle->ReadBytes = VFSWinRT_ReadBytes;
                handle->WriteBytes = VFSWinRT_WriteBytes;
                handle->Seek = VFSWinRT_Seek;
                handle->Tell = VFSWinRT_Tell;
                handle->GetLen = VFSWinRT_GetLen;
                handle->Close = VFSWinRT_Close;
                handle->Flush = VFSWinRT_Flush;
                return handle;
        }
        catch (const winrt::hresult_error &)
        {
                return NULL;
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

void Sys_mkdir(const char *path)
{
        std::wstring resolved = SysWinRT_ResolvePath(path);
        if (resolved.empty())
                return;
        std::vector<std::wstring> segments;
        if (!SysWinRT_SplitRelative(resolved, segments))
                return;
        try
        {
                auto folder = ApplicationData::Current().LocalFolder();
                for (auto &segment : segments)
                {
                        folder = folder.CreateFolderAsync(segment, CreationCollisionOption::OpenIfExists).get();
                }
        }
        catch (const winrt::hresult_error &)
        {
        }
}

qboolean Sys_rmdir(const char *path)
{
        std::wstring resolved = SysWinRT_ResolvePath(path);
        if (resolved.empty())
                return false;
        std::vector<std::wstring> segments;
        if (!SysWinRT_SplitRelative(resolved, segments) || segments.empty())
                return false;
        std::wstring leaf = segments.back();
        segments.pop_back();
        try
        {
                auto folder = ApplicationData::Current().LocalFolder();
                for (auto &segment : segments)
                        folder = folder.GetFolderAsync(segment).get();
                auto target = folder.GetFolderAsync(leaf).get();
                target.DeleteAsync(StorageDeleteOption::PermanentDelete).get();
                return true;
        }
        catch (const winrt::hresult_error &)
        {
                return false;
        }

}

qboolean Sys_remove(const char *path)
{
        std::wstring resolved = SysWinRT_ResolvePath(path);
        if (resolved.empty())
                return false;
        std::vector<std::wstring> segments;
        if (!SysWinRT_SplitRelative(resolved, segments) || segments.empty())
                return false;
        std::wstring leaf = segments.back();
        segments.pop_back();
        try
        {
                auto folder = ApplicationData::Current().LocalFolder();
                for (auto &segment : segments)
                        folder = folder.GetFolderAsync(segment).get();
                auto file = folder.GetFileAsync(leaf).get();
                file.DeleteAsync(StorageDeleteOption::PermanentDelete).get();
                return true;
        }
        catch (const winrt::hresult_error &)
        {
                return false;
        }
}

qboolean Sys_Rename(const char *oldfname, const char *newfname)
{
        std::wstring from = SysWinRT_ResolvePath(oldfname);
        std::wstring to = SysWinRT_ResolvePath(newfname);
        if (from.empty() || to.empty())
                return false;
        std::vector<std::wstring> fromSegments;
        std::vector<std::wstring> toSegments;
        if (!SysWinRT_SplitRelative(from, fromSegments) || fromSegments.empty())
                return false;
        if (!SysWinRT_SplitRelative(to, toSegments) || toSegments.empty())
                return false;
        std::wstring fromLeaf = fromSegments.back();
        fromSegments.pop_back();
        std::wstring toLeaf = toSegments.back();
        toSegments.pop_back();
        try
        {
                auto fromFolder = ApplicationData::Current().LocalFolder();
                for (auto &segment : fromSegments)
                        fromFolder = fromFolder.GetFolderAsync(segment).get();
                auto toFolder = ApplicationData::Current().LocalFolder();
                for (auto &segment : toSegments)
                        toFolder = toFolder.CreateFolderAsync(segment, CreationCollisionOption::OpenIfExists).get();

                auto item = fromFolder.TryGetItemAsync(fromLeaf).get();
                if (!item)
                        return false;
                if (auto file = item.try_as<StorageFile>())
                        file.MoveAsync(toFolder, toLeaf, NameCollisionOption::ReplaceExisting).get();
                else if (auto folder = item.try_as<StorageFolder>())
                        folder.MoveAsync(toFolder, toLeaf, NameCollisionOption::ReplaceExisting).get();
                else
                        return false;
                return true;
        }
        catch (const winrt::hresult_error &)
        {
                return false;
        }
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
