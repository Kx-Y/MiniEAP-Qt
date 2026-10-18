#ifndef _MINIEAP_PROC_UTIL_H
#define _MINIEAP_PROC_UTIL_H

#include "minieap_common.h"

/*
 * Run a shell command and block until it finishes.
 *
 * Used for --dhcp-script. POSIX hands the line to /bin/sh; Windows keeps the
 * same semantics through cmd.exe, but does so with _spawnvp instead of
 * system() so a GUI build does not flash a console window for every run.
 *
 * Return: the command's exit status, or -1 if it could not be started.
 */
int minieap_run_shell_command(const char* command);

#endif
