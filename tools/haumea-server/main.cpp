// SPDX-License-Identifier: GPL-2.0-or-later
// haumea-server: a multiplayer session on a machine that stays up, such as
// the one that runs the SA-MP/open.mp server. Editors join it from File >
// Multiplayer > Join with its address, port and password. The first editor
// into an empty session sends its map; with --state the session is kept in a
// file and survives restarts.
#include "../haumea/mpprotocol.h"	// first: winsock2.h before windows.h
#include "../haumea/mprelay.h"

#include <random>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static volatile sig_atomic_t gStop, gStopped;

static void
onSignal(int)
{
	gStop = 1;
}

#ifdef _WIN32
// Ctrl+C, closing the window, logging off, shutting down: Windows ends the
// process when this returns, so it waits for the session to be saved.
static BOOL WINAPI
onConsole(DWORD)
{
	gStop = 1;
	while(!gStopped)
		Sleep(50);
	return TRUE;
}
#endif

static void
printLine(const char *line)
{
	char stamp[32];
	time_t t = time(nullptr);
	strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", localtime(&t));
	printf("[%s] %s\n", stamp, line);
	fflush(stdout);
}

static void
usage(FILE *out)
{
	fprintf(out,
		"Usage: haumea-server [options]\n"
		"\n"
		"Hosts a Haumea multiplayer session. Editors join it with this machine's\n"
		"address, the port and the password (File > Multiplayer > Join).\n"
		"\n"
		"  --port N          TCP port to listen on (default %d; open it in the firewall)\n"
		"  --bind ADDRESS    listen on one address only, IPv4 or IPv6 (default: all of both)\n"
		"  --password TEXT   what joining needs (or HAUMEA_SERVER_PASSWORD);\n"
		"                    without one a random password is made and printed\n"
		"  --state FILE      keep the session in FILE, so it survives restarts\n"
		"  --name TEXT       the session's name in the editors (default \"Haumea server\")\n"
		"  --max-clients N   editors at once (default 32)\n"
		"  --help            this text\n",
		MP_DEFAULT_PORT_NUMBER);
}

int
main(int argc, char **argv)
{
	MpRelayOptions options;
	options.port = MP_DEFAULT_PORT_NUMBER;
	options.log = printLine;
	if(const char *password = getenv("HAUMEA_SERVER_PASSWORD"))
		options.password = password;
	for(int i = 1; i < argc; i++){
		const char *arg = argv[i];
		const char *value = i + 1 < argc ? argv[i + 1] : nullptr;
		auto need = [&](void) -> const char* {
			if(value == nullptr){
				fprintf(stderr, "haumea-server: %s needs a value\n", arg);
				exit(2);
			}
			i++;
			return value;
		};
		if(strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0){
			usage(stdout);
			return 0;
		}else if(strcmp(arg, "--port") == 0)
			options.port = atoi(need());
		else if(strcmp(arg, "--bind") == 0)
			options.bindAddress = need();
		else if(strcmp(arg, "--password") == 0)
			options.password = need();
		else if(strcmp(arg, "--state") == 0)
			options.statePath = need();
		else if(strcmp(arg, "--name") == 0)
			options.name = need();
		else if(strcmp(arg, "--max-clients") == 0)
			options.maxClients = atoi(need());
		else{
			fprintf(stderr, "haumea-server: unknown option %s\n\n", arg);
			usage(stderr);
			return 2;
		}
	}
	if(options.port < 0 || options.port > 65535 || options.maxClients < 1 || options.maxClients > 1000){
		fprintf(stderr, "haumea-server: port must be 0-65535 and max-clients 1-1000\n");
		return 2;
	}
	if(options.password.size() >= MP_PASSWORD_LEN){
		fprintf(stderr, "haumea-server: the password can be at most %d characters\n", MP_PASSWORD_LEN - 1);
		return 2;
	}
	bool generated = options.password.empty();
	if(generated){
		static const char letters[] = "ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnpqrstuvwxyz23456789";
		std::random_device random;
		for(int i = 0; i < 12; i++)
			options.password += letters[random() % (sizeof(letters) - 1)];
	}
	std::string error;
	if(!MpRelayStart(options, &error)){
		fprintf(stderr, "haumea-server: %s\n", error.c_str());
		return 1;
	}
	if(generated){
		char line[128];
		snprintf(line, sizeof(line), "password: %s (set one with --password)", options.password.c_str());
		printLine(line);
	}
	if(options.statePath.empty())
		printLine("no --state file: the session ends when the server stops");
	signal(SIGINT, onSignal);
	signal(SIGTERM, onSignal);
#ifdef _WIN32
	SetConsoleCtrlHandler(onConsole, TRUE);
#else
	signal(SIGPIPE, SIG_IGN);
#endif
	while(!gStop){
		MpRelayWait(0.25);
		MpRelayUpdate();
	}
	MpRelayStop();
	gStopped = 1;
	return 0;
}
