#include "quakedef.h"
#include "winquake.h"
#include "fs.h"

#include "sys_win_common.h"

#ifdef WINRT
#include <winrt/base.h>
#endif

char *sys_argv[MAX_NUM_ARGVS] = {};

#ifdef CATCHCRASH
#ifdef WINRT
volatile int watchdogframe = 0;
#else
extern volatile int watchdogframe;
#endif
#endif

#ifdef WINRT
HANDLE tevent = NULL;
#else
extern HANDLE tevent;
#endif

#ifndef CLIENTONLY
extern qboolean isDedicated;
#endif
extern int isPlugin;

#ifdef __cplusplus
extern "C" {
#endif
qboolean Sys_Startup_CheckMem(quakeparms_t *parms);
#ifdef __cplusplus
}
#endif

#if !defined(SERVERONLY) && !defined(WINRT)
extern void SetHookState(qboolean state);
#endif

#if !defined(SERVERONLY) && !defined(WINRT)
extern void Win7_TaskListInit(void);
#endif

int Sys_Windows_Run(quakeparms_t *parms)
{
        double oldtime;

#ifndef CLIENTONLY
        if (isDedicated)
        {
#if !defined(CLIENTONLY)
                if (!Sys_InitTerminal())
                        Sys_Error("Couldn't allocate dedicated server console");
#endif
        }
#endif

        if (!Sys_Startup_CheckMem(parms))
                Sys_Error("Not enough memory free; check disk space\n");

#ifndef CLIENTONLY
        if (isDedicated)
        {
#if !defined(CLIENTONLY)
                float delay;

                SV_Init(parms);

                delay = SV_Frame();

                while (1)
                {
                        if (!isDedicated)
                                Sys_Error("Dedicated was cleared");
                        NET_Sleep(delay, false);
                        delay = SV_Frame();
                }
#endif
                return EXIT_FAILURE;
        }
#endif

#ifdef WINRT
        tevent = CreateEventExW(nullptr, nullptr, 0, EVENT_MODIFY_STATE | SYNCHRONIZE);
#else
        tevent = CreateEvent(NULL, FALSE, FALSE, NULL);
#endif
        if (!tevent)
                Sys_Error("Couldn't create event");

#ifdef SERVERONLY
        Sys_Printf("SV_Init\n");
        SV_Init(parms);
#else
        Sys_Printf("Host_Init\n");
        Host_Init(parms);
#endif

        oldtime = Sys_DoubleTime();

#if !defined(WINRT) && !defined(MINGW)
#if _MSC_VER > 1200
        Win7_TaskListInit();
#endif
#endif

        if (isPlugin == 1)
        {
                printf("status Running!\n");
                fflush(stdout);
        }

        while (1)
        {
#ifdef CATCHCRASH
#ifndef WINRT
                watchdogframe++;
#else
                ++watchdogframe;
#endif
#endif
#ifndef CLIENTONLY
                if (isDedicated)
                {
#if !defined(CLIENTONLY)
                        float delay;
                        double newtime = Sys_DoubleTime();
                        double time = newtime - oldtime;
                        (void)time;
                        oldtime = newtime;

                        delay = SV_Frame();

                        NET_Sleep(delay, false);
#else
                        Sys_Error("wut?");
#endif
                }
                else
#endif
                {
#if !defined(SERVERONLY)
                        double newtime = Sys_DoubleTime();
                        double time = newtime - oldtime;
                        double sleeptime = Host_Frame(time);
                        oldtime = newtime;
#if !defined(WINRT) && !defined(SERVERONLY)
                        SetHookState(vid.activeapp);
#endif
                        if (sleeptime > 0.0)
                                Sys_Sleep(sleeptime);
#else
                        Sys_Error("wut?");
#endif
                }
        }

        return EXIT_FAILURE;
}
