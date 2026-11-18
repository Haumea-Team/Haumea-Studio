// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <stdio.h>
#include <string>

// Haumea Studio's updater: checks GitHub Releases for this channel, shows a
// banner (never a modal), downloads and verifies the release bundle in the
// background and installs it at the next start, only the files its manifest
// lists. The previous version is kept for Help > Roll back.

// --update-check / --update-install / --update-rollback (the tests' way in,
// no window): the exit code, or -1 when the command line has none of them.
int UpdaterCommandLine(int argc, char **argv);

// At startup, before any window: install a downloaded update and start the
// new version in this one's place (does not return then).
void UpdaterInstallStaged(void);

// After the window is up: start the background check (when allowed).
void UpdaterCheckForUpdate(void);
// Help > Check for updates.
void UpdaterCheckNow(void);
// Every frame: the banner and What's new.
void UpdaterDrawGui(void);
// Something to show (an update, or what the last one did), and the same
// text and buttons drawn in the caller's layout (the start screen).
bool UpdaterHasNews(void);
void UpdaterDrawInline(void);
// Its What's new button was pressed (it toggles): draw the notes, body only,
// in the caller's layout.
bool UpdaterWhatsNewRequested(void);
void UpdaterDrawWhatsNewInline(void);
// Help menu: Check for updates, Roll back.
void UpdaterHelpMenuItems(void);
// Preferences > Updates.
void UpdaterPreferences(void);

// settings.cfg: true when key was the updater's.
bool UpdaterParseSetting(const char *key, const char *value);
void UpdaterWriteSettings(FILE *f);

// An https file to outPath, the way updates are fetched (curl on Linux and
// macOS, WinHTTP on Windows). Blocks: call it from a worker thread.
bool HttpDownload(const std::string &url, const std::string &outPath, std::string *error);
