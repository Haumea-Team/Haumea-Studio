// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <string>

// A terminal in a panel: the user's own shell on a pseudo-terminal (a PTY, or
// ConPTY on Windows), with libvterm reading what it prints. Nothing here knows
// what runs in it, so any command-line program works: an AI assistant of any
// make, a server, an editor. The shell keeps running while the panel is closed
// and ends with the editor.

// Every frame, open or not: reads the shell's output and, when open, draws it.
// folder: where a new shell starts, "" for the home folder.
void DrawTerminalWindow(bool *open, const char *folder);

// What the terminal shows now, a line each (UI scripts).
std::string TerminalScreenText(void);
