// SPDX-License-Identifier: GPL-2.0-or-later
#include "haumea.h"
#include "i18n.h"
#include "version.h"
#include "versionutil.h"
#include "telemetry.h"
#include "workspace.h"
#include "startscreen.h"

#include <atomic>
#include <string>
#include <vector>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <winhttp.h>
#include <wincrypt.h>
#include <process.h>
#include <shlobj.h>
#else
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

// See telemetry.h. The state lives in <config>/telemetry.txt:
//   consent yes|no        (no line: not asked yet)
//   uid <32 hex>          (only while consent is yes)
//   last_sent YYYY-MM-DD  (the UTC day of the last 2xx answer)

static const char *DEFAULT_URL = "https://telemetry.itsneufox.xyz/haumea/telemetry/ping";
static const char *PRIVACY_URL = "https://telemetry.itsneufox.xyz/privacy";

static bool gLoaded;
static int gConsent = TELEMETRY_UNASKED;
static std::string gUid;
static std::string gLastSent;
static std::atomic<bool> gSending(false);
static bool gQuestionShown;

static std::string
statePath(void)
{
	return std::string(GetConfigDir()) + "/telemetry.txt";
}

static void
load(void)
{
	if(gLoaded)
		return;
	gLoaded = true;
	FILE *f = fopen(statePath().c_str(), "r");
	if(f == nil)
		return;
	char line[128], key[32], value[96];
	while(fgets(line, sizeof(line), f)){
		if(sscanf(line, "%31s %95s", key, value) != 2)
			continue;
		if(strcmp(key, "consent") == 0)
			gConsent = strcmp(value, "yes") == 0 ? TELEMETRY_YES : TELEMETRY_NO;
		else if(strcmp(key, "uid") == 0)
			gUid = value;
		else if(strcmp(key, "last_sent") == 0)
			gLastSent = value;
	}
	fclose(f);
	if(gConsent != TELEMETRY_YES){
		gUid.clear();
		gLastSent.clear();
	}
}

static void
save(void)
{
	if(SettingsAreReadOnly())
		return;
	std::string path = statePath();
	if(gConsent == TELEMETRY_UNASKED){
		remove(path.c_str());
		return;
	}
	std::string temporary = path + ".tmp";
	FILE *f = fopen(temporary.c_str(), "w");
	if(f == nil)
		return;
	fprintf(f, "consent %s\n", gConsent == TELEMETRY_YES ? "yes" : "no");
	if(gConsent == TELEMETRY_YES && !gUid.empty())
		fprintf(f, "uid %s\n", gUid.c_str());
	if(gConsent == TELEMETRY_YES && !gLastSent.empty())
		fprintf(f, "last_sent %s\n", gLastSent.c_str());
	if(fclose(f) != 0){
		remove(temporary.c_str());
		return;
	}
#ifdef _WIN32
	MoveFileExA(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
#else
	rename(temporary.c_str(), path.c_str());
#endif
}

// Builds before 1.0 kept a default-on ID in %LOCALAPPDATA%\Haumea; it goes,
// so nothing from then is ever sent.
static void
forgetOldWindowsId(void)
{
#ifdef _WIN32
	char appdata[MAX_PATH] = {};
	if(SHGetFolderPathA(nil, CSIDL_LOCAL_APPDATA, nil, 0, appdata) != S_OK)
		return;
	char path[MAX_PATH + 32];
	snprintf(path, sizeof(path), "%s\\Haumea\\uid.txt", appdata);
	DeleteFileA(path);
	snprintf(path, sizeof(path), "%s\\Haumea\\telemetry.txt", appdata);
	DeleteFileA(path);
#endif
}

static bool
randomUid(std::string *out)
{
	unsigned char bytes[16];
	bool ok = false;
#ifdef _WIN32
	HCRYPTPROV provider = 0;
	if(CryptAcquireContextA(&provider, nil, nil, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT)){
		ok = CryptGenRandom(provider, sizeof(bytes), bytes) != FALSE;
		CryptReleaseContext(provider, 0);
	}
#else
	int fd = open("/dev/urandom", O_RDONLY);
	if(fd >= 0){
		ok = read(fd, bytes, sizeof(bytes)) == (ssize_t)sizeof(bytes);
		close(fd);
	}
#endif
	if(!ok)
		return false;	// no ID from a weak source: nothing is sent
	char hex[33];
	for(int i = 0; i < 16; i++)
		snprintf(hex + i * 2, 3, "%02x", bytes[i]);
	*out = hex;
	return true;
}

static std::string
today(void)
{
	time_t now = time(nil);
	struct tm t;
#ifdef _WIN32
	gmtime_s(&t, &now);
#else
	gmtime_r(&now, &t);
#endif
	char text[16];
	strftime(text, sizeof(text), "%Y-%m-%d", &t);
	return text;
}

static const char*
osName(void)
{
#if defined(_WIN32)
	return "windows";
#elif defined(__APPLE__)
	return "macos";
#else
	return "linux";
#endif
}

static const char*
testUrl(void)
{
	const char *url = getenv("HAUMEA_TELEMETRY_URL");
	return url && url[0] ? url : nil;
}

static bool
isLoopbackHttp(const std::string &url)
{
	return url.compare(0, 17, "http://127.0.0.1:") == 0 || url.compare(0, 17, "http://localhost:") == 0;
}

static bool
hasArgument(const char *name)
{
	for(int i = 1; i < sk::args.argc; i++)
		if(strcmp(sk::args.argv[i], name) == 0)
			return true;
	return false;
}

// Why nothing may be sent from this run, or nil. N_(): logged in English,
// shown with _().
static const char*
blockedBecause(void)
{
	const char *off = getenv("HAUMEA_NO_TELEMETRY");
	if(off && off[0])
		return "HAUMEA_NO_TELEMETRY";
	if(testUrl())
		return nil;	// a test of its own server
	if(IsAutomatedRun())
		return N_("an automated run");
	if(hasArgument("--agent-socket"))
		return N_("an agent session");
	if(getenv("CI") || getenv("GITHUB_ACTIONS"))
		return "CI";
	return nil;
}

// ---- The POST: the HTTP status, or 0 without an answer ----

#ifdef _WIN32
static int
httpPost(const std::string &url, const std::string &body)
{
	std::wstring wurl(url.begin(), url.end());
	URL_COMPONENTS parts = {};
	parts.dwStructSize = sizeof(parts);
	wchar_t host[256], path[1024];
	parts.lpszHostName = host; parts.dwHostNameLength = 256;
	parts.lpszUrlPath = path; parts.dwUrlPathLength = 1024;
	if(!WinHttpCrackUrl(wurl.c_str(), 0, 0, &parts))
		return 0;
	bool secure = parts.nScheme == INTERNET_SCHEME_HTTPS;
	std::string ua = std::string("Haumea/") + HaumeaFullVersion();
	std::wstring wua(ua.begin(), ua.end());
	HINTERNET session = WinHttpOpen(wua.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
		WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
	if(session)
		WinHttpSetTimeouts(session, 10000, 10000, 15000, 15000);
	HINTERNET connect = session ? WinHttpConnect(session, host, parts.nPort, 0) : nil;
	HINTERNET request = connect ? WinHttpOpenRequest(connect, L"POST", path, nil, WINHTTP_NO_REFERER,
		WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0) : nil;
	DWORD status = 0, size = sizeof(status);
	if(request){
		DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
		WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy));
		if(!(WinHttpSendRequest(request, L"Content-Type: application/json\r\n", (DWORD)-1,
		                        (LPVOID)body.data(), (DWORD)body.size(), (DWORD)body.size(), 0) &&
		     WinHttpReceiveResponse(request, nil) &&
		     WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
		                         WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, nil)))
			status = 0;
	}
	if(request) WinHttpCloseHandle(request);
	if(connect) WinHttpCloseHandle(connect);
	if(session) WinHttpCloseHandle(session);
	return (int)status;
}
#else
// curl, as the updater uses it; the body goes on its stdin, not its command line.
static int
httpPost(const std::string &url, const std::string &body)
{
	std::string ua = std::string("Haumea/") + HaumeaFullVersion();
	bool https = url.compare(0, 8, "https://") == 0;
	std::vector<std::string> args = { "curl", "-sS", "-o", "/dev/null", "-w", "%{http_code}",
		"--connect-timeout", "10", "--max-time", "15", "--proto", https ? "=https" : "=http",
		"--max-redirs", "0", "-A", ua, "-H", "Content-Type: application/json",
		"--data-binary", "@-", url };
	std::vector<char*> argv;
	for(std::string &a : args)
		argv.push_back(&a[0]);
	argv.push_back(nil);
	int input[2], output[2];
	if(pipe(input) != 0)
		return 0;
	if(pipe(output) != 0){
		close(input[0]); close(input[1]);
		return 0;
	}
	pid_t pid = fork();
	if(pid < 0){
		close(input[0]); close(input[1]); close(output[0]); close(output[1]);
		return 0;
	}
	if(pid == 0){
		dup2(input[0], 0);
		dup2(output[1], 1);
		int null = open("/dev/null", O_WRONLY);
		if(null >= 0)
			dup2(null, 2);
		close(input[0]); close(input[1]); close(output[0]); close(output[1]);
		execvp("curl", argv.data());
		_exit(127);
	}
	close(input[0]);
	close(output[1]);
	signal(SIGPIPE, SIG_IGN);
	ssize_t written = write(input[1], body.data(), body.size());
	(void)written;
	close(input[1]);
	char code[16] = {};
	ssize_t got = read(output[0], code, sizeof(code) - 1);
	close(output[0]);
	int status = 0;
	waitpid(pid, &status, 0);
	if(got <= 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
		return 0;
	return atoi(code);
}
#endif

std::string
TelemetryInstallId(void)
{
	load();
	return gConsent == TELEMETRY_YES ? gUid : std::string();
}

int
TelemetryPostJson(const std::string &url, const std::string &body)
{
	return httpPost(url, body);
}

bool
TelemetryUrlAllowed(const std::string &url, bool isTestUrl)
{
	return url.compare(0, 8, "https://") == 0 || (isTestUrl && isLoopbackHttp(url));
}

static std::string gPostUrl, gPostBody, gPostDay;
static std::atomic<int> gLastStatus(-1);

static void
sendWorker(void)
{
	gLastStatus = httpPost(gPostUrl, gPostBody);
	gSending = false;
}

#ifdef _WIN32
static unsigned __stdcall
sendThread(void *arg)
{
	(void)arg;
	sendWorker();
	return 0;
}
#else
static void*
sendThread(void *arg)
{
	(void)arg;
	sendWorker();
	return nil;
}
#endif

// Records a finished send, on the main thread, which owns the state.
static void
collectResult(void)
{
	int status = gLastStatus.exchange(-1);
	if(status < 0)
		return;
	if(status >= 200 && status < 300){
		gLastSent = gPostDay;
		save();
		log("Telemetry: sent today's ping (%d)\n", status);
	}else
		log("Telemetry: the ping wasn't taken (%s); not retried\n",
			status ? ("HTTP " + std::to_string(status)).c_str() : "no answer");
}

int
TelemetryGetConsent(void)
{
	load();
	return gConsent;
}

bool
TelemetryIsEnabled(void)
{
	return TelemetryGetConsent() == TELEMETRY_YES;
}

void
TelemetrySendPing(void)
{
	load();
	forgetOldWindowsId();
	collectResult();
	if(gConsent != TELEMETRY_YES || gSending)
		return;
	if(const char *why = blockedBecause()){
		log("Telemetry: not in %s\n", why);
		return;
	}
	std::string day = today();
	if(gLastSent == day)
		return;	// at most once a day
	std::string url = testUrl() ? testUrl() : DEFAULT_URL;
	if(!TelemetryUrlAllowed(url, testUrl() != nil)){
		log("Telemetry: refusing %s (https only)\n", url.c_str());
		return;
	}
	if(gUid.empty()){
		if(!randomUid(&gUid))
			return;
		save();
	}
	char body[256];
	snprintf(body, sizeof(body), "{\"uid\":\"%s\",\"version\":\"%s\",\"channel\":\"%s\",\"os\":\"%s\"}",
		gUid.c_str(), HaumeaFullVersion(), HAUMEA_CHANNEL, osName());
	gPostUrl = url;
	gPostBody = body;
	gPostDay = day;
	gSending = true;
#ifdef _WIN32
	HANDLE h = (HANDLE)_beginthreadex(nil, 0, sendThread, nil, 0, nil);
	if(h)
		CloseHandle(h);
	else
		gSending = false;
#else
	pthread_t t;
	if(pthread_create(&t, nil, sendThread, nil) == 0)
		pthread_detach(t);
	else
		gSending = false;
#endif
}

void
TelemetrySetEnabled(bool enabled)
{
	load();
	gConsent = enabled ? TELEMETRY_YES : TELEMETRY_NO;
	if(!enabled){
		gUid.clear();	// a new ID if it is ever turned on again
		gLastSent.clear();
	}
	save();
	log("Telemetry: %s\n", enabled ? "on" : "off; the install ID is deleted");
	if(enabled)
		TelemetrySendPing();
}

void
TelemetryUpdate(void)
{
	if(!gSending)
		collectResult();
}

void
TelemetryDrawConsent(void)
{
	if(TelemetryGetConsent() != TELEMETRY_UNASKED)
		return;
	gQuestionShown = true;
	ImGui::Spacing();
	// A quiet card tinted with the accent, not a box that shouts.
	ImVec4 accent = ImGui::GetStyleColorVec4(ImGuiCol_CheckMark);
	ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(accent.x, accent.y, accent.z, 0.10f));
	ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.0f);
	ImGui::BeginChild("##telemetry_question", ImVec2(0, 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
	ImGui::TextWrapped(_("Help count Haumea Studio's users? Once a day it would send a random install ID, "
		"the version, the channel and the operating system: nothing about you, your computer or your maps."));
	if(ImGui::Button(_("Yes, send it")))
		TelemetrySetEnabled(true);
	ImGui::SameLine();
	if(ImGui::Button(_("No thanks")))
		TelemetrySetEnabled(false);
	ImGui::SameLine();
	if(ImGui::SmallButton(_("What is kept?")))
		OpenInFileManager(PRIVACY_URL);
	ImGui::SameLine();
	ImGui::TextDisabled(_("Preferences > Privacy changes it later."));
	ImGui::EndChild();
	ImGui::PopStyleVar();
	ImGui::PopStyleColor();
}

void
TelemetryStartScreenClosed(void)
{
	if(gQuestionShown && TelemetryGetConsent() == TELEMETRY_UNASKED)
		TelemetrySetEnabled(false);	// not answered: no
	gQuestionShown = false;
}

void
TelemetryPreferences(void)
{
	bool on = TelemetryIsEnabled();
	if(ImGui::Checkbox(_("Send a daily usage ping"), &on))
		TelemetrySetEnabled(on);
	ImGui::TextWrapped(_("At most once a day: a random install ID, the version (%s), the channel (%s) and the "
		"operating system (%s). It counts how many installs are in use; nothing about you, your computer or "
		"your maps is sent. Turning it off deletes the ID."), HaumeaFullVersion(), HAUMEA_CHANNEL, osName());
	if(on){
		load();
		if(!gLastSent.empty())
			ImGui::TextDisabled(_("Last sent %s (UTC)."), gLastSent.c_str());
		if(const char *why = blockedBecause())
			ImGui::TextDisabled(_("Not sent from this run (%s)."), _(why));
	}
	if(ImGui::SmallButton(_("What is sent and kept...")))
		OpenInFileManager(PRIVACY_URL);
}
