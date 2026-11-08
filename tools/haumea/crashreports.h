// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// Crash and error reports. When Haumea crashes, a handler (POSIX signals;
// SetUnhandledExceptionFilter on Windows) writes what happened into a file
// opened at start, with only async-signal-safe calls; the next start turns
// it into <config>/reports/pending/<id>.json: the version, commit, channel,
// OS, graphics, the stack as module+offset, the last log lines and the mode
// and window, with the home folder, user name, game folder, workspace and
// map names replaced by placeholders. Errors the editor survives are
// reported the same way (CrashReportsError), a few per session.
// Sending asks first (a banner at the next start: View report, Send, Don't
// send, Always send; Preferences > Privacy: Ask, Always or Never), then POSTs
// JSON (at most 256 KB) to https://reports.itsneufox.xyz/haumea/reports; a 2xx
// deletes the report, and none is kept past 30 days. A report carries the
// install ID only while the daily ping is on. Never sent from tests, UI
// scripts, command-line modes, agent sessions or CI, nor with HAUMEA_NO_REPORTS;
// HAUMEA_REPORTS_URL points it at a test server (loopback may be http).
// The schema is tools/telemetry/haumea_telemetry.py's validate_report.

// At start, once the config folder is known: the handlers.
void CrashReportsInstall(void);
// Once graphics are up: the renderer's name, and the libraries loaded since.
void CrashReportsNoteGraphics(void);
// After that: last run's crash becomes a pending report; old ones go;
// with Always they are sent. --crash-test (with HAUMEA_CRASH_TEST=1) crashes here.
void CrashReportsStartup(void);
// Clean exit: nothing to report.
void CrashReportsShutdown(void);

// Every log line, kept for a report (the last 200).
void CrashReportsLog(const char *text);
// Every frame: the mode and focused window a report names; sending results.
void CrashReportsSetContext(const char *mode, const char *window);
void CrashReportsUpdate(void);
// An error the editor goes on after: one report per kind (stack), a few per session.
void CrashReportsError(const char *where, const char *fmt, ...);

// The banner at start while reports wait for an answer, and its viewer.
void CrashReportsDrawBanner(void);
// Preferences > Privacy.
void CrashReportsPreferences(void);
