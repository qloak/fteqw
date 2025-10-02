#include "quakedef.h"
#include "sys_win_common.h"

#include <winrt/Windows.ApplicationModel.Core.h>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.UI.Core.h>

#include <windows.h>

using namespace winrt;
using namespace Windows::ApplicationModel;
using namespace Windows::ApplicationModel::Core;
using namespace Windows::UI::Core;

extern void SysWinRT_AttachWindow(CoreWindow const &window);
extern void SysWinRT_SetupApplicationEvents();
extern void SysWinRT_InitRuntime();

namespace FTEWinRT
{
struct App : winrt::implements<App, IFrameworkViewSource, IFrameworkView>
{
        CoreApplicationView m_view{nullptr};
        bool m_windowAttached{false};
        winrt::event_revoker<CoreApplicationView> m_activated{};

        IFrameworkView CreateView()
        {
                return *this;
        }

        void Initialize(CoreApplicationView const &view)
        {
                m_view = view;
                winrt::init_apartment(winrt::apartment_type::single_threaded);
                SysWinRT_SetupApplicationEvents();
                SysWinRT_InitRuntime();
                m_activated = view.Activated(winrt::auto_revoke,
                                             [](CoreApplicationView const &, IActivatedEventArgs const &)
                                             {
                                                     auto window = CoreWindow::GetForCurrentThread();
                                                     if (window)
                                                             window.Activate();
                                             });
        }

        void SetWindow(CoreWindow const &window)
        {
                SysWinRT_AttachWindow(window);
                m_windowAttached = true;
        }

        void Load(hstring const &)
        {
        }

        void Run()
        {
                if (!m_windowAttached)
                {
                        auto window = CoreWindow::GetForCurrentThread();
                        if (window)
                                SysWinRT_AttachWindow(window);
                }

                static char bindir[MAX_OSPATH];
                static char basedir[MAX_OSPATH];

                wchar_t modulePath[MAX_PATH] = {};
                bindir[0] = '\0';
                if (GetModuleFileNameW(nullptr, modulePath, ARRAYSIZE(modulePath)))
                {
                        narrowen(bindir, sizeof(bindir), modulePath);
                }

                quakeparms_t parms{};
                parms.binarydir = bindir;

                int argc = Sys_ProcessCommandline(sys_argv, MAX_NUM_ARGVS, bindir);
                parms.argc = argc;
                parms.argv = (const char **)sys_argv;

                char *slash = COM_SkipPath(bindir);
                if (slash)
                        *slash = '\0';
                Q_strncpyz(basedir, bindir, sizeof(basedir));
                parms.basedir = basedir;

                COM_InitArgv(parms.argc, parms.argv);

                (void)Sys_Windows_Run(&parms);
        }

        void Uninitialize()
        {
                if (m_activated)
                        m_activated.revoke();
        }
};
} // namespace FTEWinRT

extern "C" int __stdcall wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
        CoreApplication::Run(winrt::make<FTEWinRT::App>());
        return 0;
}
