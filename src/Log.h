/* Log.h
 * tetherd -- userspace RNDIS USB tethering daemon for macOS.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <stdio.h>

extern bool gVerbose;

void logLine(const char *level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

#define LOG_INFO(...)  logLine("info", __VA_ARGS__)
#define LOG_ERROR(...) logLine("error", __VA_ARGS__)
#define LOG_DEBUG(...) do { if (gVerbose) logLine("debug", __VA_ARGS__); } while (0)
