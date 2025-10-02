#pragma once

#include "quakedef.h"

#ifdef _WIN32
#include <windows.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

extern char *sys_argv[MAX_NUM_ARGVS];

#ifdef CATCHCRASH
extern volatile int watchdogframe;
#endif

#ifdef _WIN32
extern HANDLE tevent;
#endif

int Sys_Windows_Run(quakeparms_t *parms);
int Sys_ProcessCommandline(char **argv, int maxargc, char *argv0);

#ifdef __cplusplus
}
#endif
