// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <string>

// Downloads the latest open.mp server and/or streamer plugin into a folder,
// in the background (one at a time).
enum {
	SERVER_DOWNLOAD_IDLE,
	SERVER_DOWNLOAD_RUNNING,
	SERVER_DOWNLOAD_DONE,	// message: the release tag
	SERVER_DOWNLOAD_FAILED,	// message: why
};

// What to fetch.
enum {
	SERVER_DOWNLOAD_OPENMP = 1,
	SERVER_DOWNLOAD_STREAMER = 2,	// plugins/streamer.so/.dll and qawno/include/streamer.inc
};

// Who asked, so only they report it.
enum {
	SERVER_DOWNLOAD_FOR_WORKSPACE,
	SERVER_DOWNLOAD_FOR_TEST_SERVER,
};

// Whether dir has a server executable (open.mp or SA-MP) already.
bool ServerDownloadHasServer(const char *dir);
void ServerDownloadStart(const char *dir, int what = SERVER_DOWNLOAD_OPENMP | SERVER_DOWNLOAD_STREAMER,
	int owner = SERVER_DOWNLOAD_FOR_WORKSPACE);
// The state, the folder, the message and how much of the archive is in.
int ServerDownloadStatus(std::string *dir, std::string *message, float *megabytes);
int ServerDownloadOwner(void);
// After a finished download was shown: back to idle.
void ServerDownloadAcknowledge(void);
