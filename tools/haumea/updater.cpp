// SPDX-License-Identifier: GPL-2.0-or-later
#include "haumea.h"
#include "i18n.h"
#include "version.h"
#include "versionutil.h"
#include "updatecore.h"
#include "updater.h"
#include "textdraws.h"
#include "testserver.h"
#include "agentbridge.h"
#include "workspace.h"
#include "icons.h"
#include "startscreen.h"
#include "namaka.h"

#include <atomic>
#include <string>
#include <sys/stat.h>

#ifdef _WIN32
#include <winhttp.h>
#include <process.h>
#pragma comment(lib, "winhttp.lib")
#else
#include <pthread.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

// The flow: a background check reads this channel's releases; a newer one
// shows the banner. Update now (or "download automatically") fetches
// SHA256SUMS and this platform's bundle, checks the bundle's sha256, unpacks
// it into <config>/update/staged and checks every file against its MANIFEST.
// The next start (or Restart now) installs it before any window opens:
// UpdateInstall moves only manifest files, keeping the old ones for Roll back.
// Integrity, not authenticity (see updatecore.cpp).

// ---- Settings ----

static bool gUpdateCheckAuto = true;
static bool gUpdateDownloadAuto = false;
// Which releases to follow: "production" (everyone) or "beta" (opt-in: the
// newest of beta and production). A beta build starts on beta; every other
// build (production, a local or CI "dev" one) on production.
static char gUpdateChannel[32];
static const char *buildChannel(void);

static void
defaultChannel(void)
{
	if(gUpdateChannel[0] == '\0')
		snprintf(gUpdateChannel, sizeof(gUpdateChannel), "%s",
			strcmp(buildChannel(), "beta") == 0 ? "beta" : "production");
}

static bool
followsBeta(void)
{
	defaultChannel();
	return strcmp(gUpdateChannel, "beta") == 0;
}

static std::vector<std::string>
followedChannels(bool beta)
{
	if(beta)
		return { "beta", "production" };
	return { "production" };
}

// This build's own channel: a release's, or "dev" (a main push) / "pr" (a pull
// request) for CI artifacts. HAUMEA_UPDATE_BUILD stands in for it in tests.
static const char*
buildChannel(void)
{
	const char *test = getenv("HAUMEA_UPDATE_BUILD");
	return test && test[0] ? test : HAUMEA_CHANNEL;
}

// A CI artifact, not a release: it updates to the official releases, the
// official build of its own commit (the same version) included.
static bool
unofficialBuild(void)
{
	return strcmp(buildChannel(), "dev") == 0 || strcmp(buildChannel(), "pr") == 0;
}
static char gUpdateSkip[32];	// "Skip this version"

bool
UpdaterParseSetting(const char *key, const char *value)
{
	if(strcmp(key, "update_check") == 0)
		gUpdateCheckAuto = atoi(value) != 0;
	else if(strcmp(key, "update_download") == 0)
		gUpdateDownloadAuto = atoi(value) != 0;
	else if(strcmp(key, "update_channel") == 0)
		// Only production and beta exist; any older channel name means
		// production.
		snprintf(gUpdateChannel, sizeof(gUpdateChannel), "%s", strcmp(value, "beta") == 0 ? "beta" : "production");
	else if(strcmp(key, "update_skip") == 0)
		snprintf(gUpdateSkip, sizeof(gUpdateSkip), "%s", value);
	else
		return false;
	return true;
}

void
UpdaterWriteSettings(FILE *f)
{
	fprintf(f, "update_check %d\n", gUpdateCheckAuto ? 1 : 0);
	fprintf(f, "update_download %d\n", gUpdateDownloadAuto ? 1 : 0);
	defaultChannel();
	fprintf(f, "update_channel %s\n", gUpdateChannel);
	if(gUpdateSkip[0])
		fprintf(f, "update_skip %s\n", gUpdateSkip);
}

// ---- Paths ----

static std::string
updateDir(void)
{
	return std::string(GetConfigDir()) + "/update";
}

static std::string
installDir(void)
{
	char exe[1024];
	if(!GetHaumeaExecutablePath(exe, sizeof(exe)))
		return std::string();
	std::string path = exe;
	size_t slash = path.find_last_of("/\\");
	return slash == std::string::npos ? std::string(".") : path.substr(0, slash);
}

// Why this install can only be told about updates, or "" when it can update.
// N_(): it is also written to the notice file; shown with _().
static const char*
notifyOnlyReason(const std::string &dir)
{
	if(UpdateBundleName() == nil)
		return N_("Updates for this platform are downloaded from the release page.");
	if(dir.empty())
		return N_("Haumea can't find its own folder.");
#ifdef __linux__
	if(dir.compare(0, 5, "/usr/") == 0 || dir.compare(0, 5, "/opt/") == 0 || dir.compare(0, 5, "/nix/") == 0)
		return N_("Haumea was installed by a package manager: update it there.");
#endif
	if(UpdateFileExists(dir + "/.package-managed"))
		return N_("Haumea was installed by a package manager: update it there.");
	if(!UpdateDirWritable(dir))
		return N_("Haumea's folder can't be written to.");
	return "";
}

// The test suite points the updater at a local release server; nothing else
// may use plain http.
static const char*
testServer(void)
{
	const char *url = getenv("HAUMEA_UPDATE_URL");
	return url && url[0] ? url : nil;
}

static std::string
releaseListUrl(void)
{
	if(const char *test = testServer())
		return std::string(test) + "/releases";
	return "https://api.github.com/repos/itsneufox/Haumea-Studio/releases?per_page=30";
}

static bool
urlAllowed(const std::string &url)
{
	if(url.compare(0, 8, "https://") == 0)
		return true;
	const char *test = testServer();
	return test && url.compare(0, strlen(test), test) == 0;
}

// ---- Threads ----

static void
startThread(void (*fn)(void))
{
#ifdef _WIN32
	struct Trampoline {
		static unsigned __stdcall run(void *arg){ ((void (*)(void))arg)(); return 0; }
	};
	HANDLE h = (HANDLE)_beginthreadex(nil, 0, Trampoline::run, (void*)fn, 0, nil);
	if(h) CloseHandle(h);
#else
	struct Trampoline {
		static void *run(void *arg){ ((void (*)(void))arg)(); return nil; }
	};
	pthread_t t;
	if(pthread_create(&t, nil, Trampoline::run, (void*)fn) == 0)
		pthread_detach(t);
#endif
}

// ---- Downloads ----

static std::atomic<long long> gDownloadedBytes(0);

#ifdef _WIN32
static bool
httpFetch(const std::string &url, const std::string &outPath, std::string *error)
{
	if(!urlAllowed(url)){
		*error = "refusing a non-https address";
		return false;
	}
	std::wstring wurl(url.begin(), url.end());
	URL_COMPONENTS parts = {};
	parts.dwStructSize = sizeof(parts);
	wchar_t host[256], path[2048];
	parts.lpszHostName = host; parts.dwHostNameLength = 256;
	parts.lpszUrlPath = path; parts.dwUrlPathLength = 2048;
	wchar_t extra[1024];
	parts.lpszExtraInfo = extra; parts.dwExtraInfoLength = 1024;
	if(!WinHttpCrackUrl(wurl.c_str(), 0, 0, &parts)){
		*error = "bad address " + url;
		return false;
	}
	std::wstring fullPath = std::wstring(path) + extra;
	bool secure = parts.nScheme == INTERNET_SCHEME_HTTPS;
	std::string ua = std::string("Haumea-Updater/") + HaumeaFullVersion();
	std::wstring wua(ua.begin(), ua.end());
	HINTERNET session = WinHttpOpen(wua.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
		WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
	HINTERNET connect = session ? WinHttpConnect(session, host, parts.nPort, 0) : nil;
	HINTERNET request = connect ? WinHttpOpenRequest(connect, L"GET", fullPath.c_str(), nil,
		WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0) : nil;
	bool ok = false;
	DWORD status = 0, statusSize = sizeof(status);
	if(request){
		// GitHub sends release assets through a redirect (https to https).
		DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
		WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy));
		ok = WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
			WinHttpReceiveResponse(request, nil) &&
			WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
				WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, nil) && status == 200;
	}
	if(!ok)
		*error = status ? "the server answered " + std::to_string(status) : std::string("no connection");
	FILE *f = ok ? fopen(outPath.c_str(), "wb") : nil;
	if(ok && f == nil){
		*error = "can't write " + outPath;
		ok = false;
	}
	if(ok){
		char buf[65536];
		DWORD got = 0;
		for(;;){
			if(!WinHttpReadData(request, buf, sizeof(buf), &got)){
				ok = false;
				*error = "the download broke off";
				break;
			}
			if(got == 0)
				break;
			if(fwrite(buf, 1, got, f) != got){
				ok = false;
				*error = "can't write " + outPath;
				break;
			}
			gDownloadedBytes += got;
		}
		ok = fclose(f) == 0 && ok;
	}
	if(request) WinHttpCloseHandle(request);
	if(connect) WinHttpCloseHandle(connect);
	if(session) WinHttpCloseHandle(session);
	if(!ok)
		remove(outPath.c_str());
	return ok;
}
#else
// curl, run as a program: every Linux and macOS install has it, and it
// handles proxies and certificates the way the system does.
static bool
httpFetch(const std::string &url, const std::string &outPath, std::string *error)
{
	if(!urlAllowed(url)){
		*error = "refusing a non-https address";
		return false;
	}
	bool https = url.compare(0, 8, "https://") == 0;
	std::string ua = std::string("Haumea-Updater/") + HaumeaFullVersion();
	std::vector<std::string> args = { "curl", "-fsSL", "--connect-timeout", "15", "--max-time", "1800",
		"--proto", https ? "=https" : "=http", "--proto-redir", https ? "=https" : "=http",
		"-A", ua, "-o", outPath, url };
	std::vector<char*> argv;
	for(std::string &a : args)
		argv.push_back(&a[0]);
	argv.push_back(nil);
	remove(outPath.c_str());
	pid_t pid = fork();
	if(pid < 0){
		*error = "can't start curl";
		return false;
	}
	if(pid == 0){
		execvp("curl", argv.data());
		_exit(127);
	}
	long long last = 0;
	int status = 0;
	for(;;){
		pid_t done = waitpid(pid, &status, WNOHANG);
		struct stat st;
		if(stat(outPath.c_str(), &st) == 0 && st.st_size > last){
			gDownloadedBytes += st.st_size - last;
			last = st.st_size;
		}
		if(done == pid)
			break;
		if(done < 0){
			*error = "lost curl";
			return false;
		}
		usleep(100000);
	}
	int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
	if(code == 0)
		return true;
	remove(outPath.c_str());
	*error = code == 127 ? "curl isn't installed" :
		code == 22 ? "the server refused the download" :
		code == 6 || code == 7 || code == 28 ? "no connection" : "the download failed (curl " + std::to_string(code) + ")";
	return false;
}
#endif

bool
HttpDownload(const std::string &url, const std::string &outPath, std::string *error)
{
	return httpFetch(url, outPath, error);
}

static bool
httpGetString(const std::string &url, const std::string &tempPath, std::string *out, std::string *error)
{
	bool ok = httpFetch(url, tempPath, error) && UpdateReadFile(tempPath, out);
	remove(tempPath.c_str());
	return ok;
}

// ---- Checking, downloading, staging ----

enum UpdateStatus {
	UPD_IDLE,
	UPD_CHECKING,
	UPD_UP_TO_DATE,
	UPD_OFFLINE,
	UPD_AVAILABLE,
	UPD_DOWNLOADING,
	UPD_READY,
	UPD_FAILED,
};

// Releases larger than this, unpacked, are refused.
static const size_t MAX_UNPACKED_BYTES = (size_t)1024 * 1024 * 1024;

static int
checkReleases(const std::string &work, const std::vector<std::string> &channels, const HaumeaVersion &current,
	const char *skip, bool allowOlder,
	UpdateRelease *rel, std::string *error)
{
	std::string json;
	if(!UpdateMakeDirs(work) || !httpGetString(releaseListUrl(), work + "/releases.json", &json, error))
		return UPD_OFFLINE;
	error->clear();
	if(UpdatePickRelease(json, channels, current, skip, allowOlder, unofficialBuild(), rel, error))
		return UPD_AVAILABLE;
	return error->empty() ? UPD_UP_TO_DATE : UPD_FAILED;
}

static bool
moveDir(const std::string &src, const std::string &dst)
{
#ifdef _WIN32
	return MoveFileExA(src.c_str(), dst.c_str(), 0) != 0;
#else
	return rename(src.c_str(), dst.c_str()) == 0;
#endif
}

// Download this platform's bundle, check it against SHA256SUMS, unpack it
// and check it against its MANIFEST, then make it work/staged.
static bool
downloadAndStage(const std::string &work, const UpdateRelease &rel, bool goBack, std::string *error,
	bool *badChecksum)
{
	*badChecksum = false;
	const char *bundle = UpdateBundleName();
	const UpdateAsset *asset = bundle ? rel.Find(bundle) : nil;
	const UpdateAsset *sumsAsset = rel.Find("SHA256SUMS");
	if(asset == nil || sumsAsset == nil){
		*error = "the release has no " + std::string(bundle ? bundle : "bundle") + " or SHA256SUMS";
		return false;
	}
	std::string downloads = work + "/download";
	if(!UpdateMakeDirs(downloads)){
		*error = "can't make " + downloads;
		return false;
	}
	std::string sums, expected;
	if(!httpGetString(sumsAsset->url, downloads + "/SHA256SUMS", &sums, error))
		return false;
	if(!UpdateSumFor(sums, bundle, &expected)){
		*error = "SHA256SUMS doesn't list " + std::string(bundle);
		*badChecksum = true;
		return false;
	}
	std::string part = downloads + "/" + bundle + ".part";
	gDownloadedBytes = 0;
	if(!httpFetch(asset->url, part, error))
		return false;
	std::string actual;
	if(!Sha256File(part, &actual) || actual != expected){
		remove(part.c_str());
		*error = "the download doesn't match SHA256SUMS";
		*badChecksum = true;
		return false;
	}
	std::string unpacked = work + "/staged.new";
	UpdateRemoveTree(unpacked);
	bool ok = UpdateUnpackTarGz(part, unpacked, MAX_UNPACKED_BYTES, error);
	remove(part.c_str());
	if(!ok)
		return false;
	std::vector<ManifestEntry> manifest;
	if(!UpdateReadManifest(unpacked + "/MANIFEST", &manifest, error)){
		UpdateRemoveTree(unpacked);
		return false;
	}
	for(const ManifestEntry &e : manifest){
		std::string hex;
		if(!Sha256File(unpacked + "/" + e.path, &hex) || hex != e.sha256){
			UpdateRemoveTree(unpacked);
			*error = e.path + " doesn't match the manifest";
			*badChecksum = true;
			return false;
		}
	}
	// What the next start needs to know: the version and its notes.
	// What the next start needs to know: the version, its page, and whether
	// it may be older than the running one (Go back to Production).
	if(!UpdateWriteFile(unpacked + "/.haumea-release",
			rel.version + "\n" + rel.pageUrl + "\n" + (goBack ? "go-back\n" : "")) ||
	   !UpdateWriteFile(unpacked + "/.haumea-notes", rel.body)){
		UpdateRemoveTree(unpacked);
		*error = "can't write the staged update";
		return false;
	}
	UpdateRemoveTree(work + "/staged");
	if(!moveDir(unpacked, work + "/staged")){
		UpdateRemoveTree(unpacked);
		*error = "can't stage the update";
		return false;
	}
	return true;
}

static std::string
stagedVersion(const std::string &staged, bool *goBack = nil)
{
	std::string text;
	if(!UpdateReadFile(staged + "/.haumea-release", &text))
		return std::string();
	if(goBack)
		*goBack = text.find("\ngo-back\n") != std::string::npos;
	return text.substr(0, text.find('\n'));
}

// ---- When updating is allowed at all ----

static bool
hasArg(const char *arg)
{
	for(int i = 1; i < sk::args.argc; i++)
		if(strcmp(sk::args.argv[i], arg) == 0)
			return true;
	return false;
}

// Only an interactive start looks for, downloads or installs updates: never
// the tests, UI scripts, round trips or CI. The one exception is a UI script
// run against a test release server (test_updater.py drives the banner).
static bool
updatesAllowed(void)
{
	const char *off = getenv("HAUMEA_NO_UPDATE");
	if(off && off[0] && strcmp(off, "0") != 0)
		return false;
	bool script = UiScriptActive() || hasArg("--ui-script");
	return !SettingsAreReadOnly() && (!script || testServer()) &&
		!hasArg("--textdraw-roundtrip") && !hasArg("--pawn-roundtrip");
}

// What stops Restart now (the update then waits for the next start), or nil.
static const char*
restartBlocker(void)
{
	if(PawnMapHasUnsavedChanges() || TextDrawFileHasUnsavedChanges())
		return _("Save your changes first.");
	if(InTextDrawMode())
		return _("Leave TextDraw mode first.");
	if(TestServerIsRunning())
		return _("Stop Test in game first.");
	if(AgentBridgeSessionActive())
		return _("An agent edit session is open.");
	return nil;
}

// ---- After a restart: what the previous run did ----

// <config>/update/notice, written before the restart, read once:
//   updated <new> <old>, then "local <path>" lines (files kept as .local)
//   rolledback <from> <to>
//   failed <message>
static std::string gNoticeKind, gNoticeVersion, gNoticeOther, gNoticeError;
static std::vector<std::string> gNoticeLocal;
static std::string gNotes;	// What's new for the banner's version

static void
writeNotice(const std::string &text)
{
	UpdateMakeDirs(updateDir());
	UpdateWriteFile(updateDir() + "/notice", text);
}

static void
readNotice(void)
{
	std::string path = updateDir() + "/notice", text;
	if(!UpdateReadFile(path, &text))
		return;
	remove(path.c_str());
	size_t at = 0;
	while(at < text.size()){
		size_t eol = text.find('\n', at);
		if(eol == std::string::npos) eol = text.size();
		std::string line = text.substr(at, eol - at);
		at = eol + 1;
		size_t sp = line.find(' ');
		std::string word = line.substr(0, sp), rest = sp == std::string::npos ? "" : line.substr(sp + 1);
		if(word == "updated" || word == "rolledback"){
			gNoticeKind = word;
			size_t sp2 = rest.find(' ');
			gNoticeVersion = rest.substr(0, sp2);
			gNoticeOther = sp2 == std::string::npos ? "" : rest.substr(sp2 + 1);
		}else if(word == "local")
			gNoticeLocal.push_back(rest);
		else if(word == "failed"){
			gNoticeKind = word;
			gNoticeError = rest;
		}
	}
	// An "updated" notice is only this version's (not a stale one).
	if(gNoticeKind == "updated" && gNoticeVersion != HaumeaFullVersion())
		gNoticeKind.clear();
	if(gNoticeKind == "updated")
		UpdateReadFile(updateDir() + "/notes", &gNotes);
}

// ---- Startup: install what was downloaded ----

void
UpdaterInstallStaged(void)
{
	// Never from a UI script, even against a test server: installing ends in
	// starting a new editor.
	if(!updatesAllowed() || UiScriptActive() || hasArg("--ui-script"))
		return;
	std::string staged = updateDir() + "/staged";
	if(!UpdateIsDirectory(staged))
		return;
	bool goBack = false;
	std::string version = stagedVersion(staged, &goBack);
	HaumeaVersion v;
	// Never older than this one, unless the user asked to go back; the same
	// version only replaces a CI artifact (its commit's official build).
	int cmp = ParseHaumeaVersion(version.c_str(), &v) ? CompareHaumeaVersion(v, HaumeaCurrentVersion()) : 0;
	if((cmp == 0 && !unofficialBuild() && !goBack) || (cmp < 0 && !goBack)){
		UpdateRemoveTree(staged);
		return;
	}
	std::string dir = installDir();
	std::string reason = notifyOnlyReason(dir);
	if(!reason.empty()){
		UpdateRemoveTree(staged);
		writeNotice("failed " + reason + "\n");
		return;
	}
	std::string notes;
	UpdateReadFile(staged + "/.haumea-notes", &notes);
	UpdateInstallResult result;
	std::string error;
	if(!UpdateInstall(staged, dir, &result, &error)){
		UpdateRemoveTree(staged);
		writeNotice("failed " + error + "\n");
		log("Update to %s failed: %s\n", version.c_str(), error.c_str());
		return;
	}
	// The version Roll back returns to.
	UpdateWriteFile(dir + "/.haumea-previous/VERSION", std::string(HaumeaFullVersion()) + "\n");
	std::string notice = "updated " + version + " " + HaumeaFullVersion() + "\n";
	for(const std::string &p : result.keptLocal)
		notice += "local " + p + "\n";
	writeNotice(notice);
	UpdateWriteFile(updateDir() + "/notes", notes);
	UpdateRemoveTree(staged);
	log("Updated to %s; starting it\n", version.c_str());
	ExecHaumeaInPlace();
	log("Couldn't start the new version; it starts next time\n");
}

static std::string
previousVersion(void)
{
	std::string text;
	if(!UpdateReadFile(installDir() + "/.haumea-previous/VERSION", &text))
		return std::string();
	return text.substr(0, text.find('\n'));
}

// ---- The background check and download ----

// The worker writes the strings, then publishes them by setting gStatus;
// the GUI reads them only after seeing that status. One worker at a time.
static std::atomic<int> gStatus(UPD_IDLE);
static std::atomic<bool> gWorking(false);
static UpdateRelease gRelease;
static std::string gError;
static const char *gNotifyOnly = "";	// a literal from notifyOnlyReason, or ""
static bool gManualCheck;		// Help > Check for updates: also say "up to date"
static bool gBannerHidden;		// Later
static bool gShowWhatsNew;
static bool gGoingBack;			// Go back to the latest Production: may be older
static bool gDrawingInline;			// inside UpdaterDrawInline
static bool gWhatsNewInline;		// its What's new, for the caller to draw

static void
downloadWorker(void)
{
	std::string error;
	bool badChecksum;
	bool ok = downloadAndStage(updateDir(), gRelease, gGoingBack, &error, &badChecksum);
	gError = error;
	// Free for the next job before the GUI can see this status and act on it.
	gWorking = false;
	gStatus = ok ? UPD_READY : UPD_FAILED;
}

static void
checkWorker(void)
{
	UpdateRelease rel;
	std::string error;
	// Going back takes the newest release of what it follows, even an older one.
	int status = gGoingBack ?
		checkReleases(updateDir(), followedChannels(followsBeta()), HaumeaCurrentVersion(), "", true, &rel, &error) :
		checkReleases(updateDir(), followedChannels(followsBeta()), HaumeaCurrentVersion(), gUpdateSkip, false,
			&rel, &error);
	gRelease = rel;
	gError = error;
	gNotifyOnly = status == UPD_AVAILABLE ? notifyOnlyReason(installDir()) : "";
	if(status == UPD_AVAILABLE && !gNotifyOnly[0] && UpdateBundleName() && !rel.Find(UpdateBundleName()))
		gNotifyOnly = N_("This release has no download for this platform.");
	// Already downloaded (the banner was put off with Later last time).
	if(status == UPD_AVAILABLE && stagedVersion(updateDir() + "/staged") == rel.version){
		gWorking = false;
		gStatus = UPD_READY;
		return;
	}
	if(status == UPD_AVAILABLE && !gNotifyOnly[0] && (gGoingBack || (gUpdateDownloadAuto && !gManualCheck))){
		gStatus = UPD_DOWNLOADING;
		downloadWorker();
		return;
	}
	gWorking = false;
	gStatus = status;
}

static void
startCheck(bool manual)
{
	if(gWorking.exchange(true))
		return;
	gGoingBack = false;
	gManualCheck = manual;
	gBannerHidden = false;
	gStatus = UPD_CHECKING;
	startThread(checkWorker);
}

// Preferences > Updates: a beta build switched back to Production gets the
// newest production release now, older or not.
static void
startGoBack(void)
{
	if(gWorking.exchange(true))
		return;
	gGoingBack = true;
	gManualCheck = true;
	gBannerHidden = false;
	gStatus = UPD_CHECKING;
	startThread(checkWorker);
}

static void
startDownload(void)
{
	if(gWorking.exchange(true))
		return;
	gStatus = UPD_DOWNLOADING;
	gDownloadedBytes = 0;
	startThread(downloadWorker);
}

void
UpdaterCheckForUpdate(void)
{
	if(!updatesAllowed())
		return;
	readNotice();
	// The first start of a new version: bring Namaka's install up to the
	// wheel this version ships (it runs in the background, offline when it can).
	if(gNoticeKind == "updated" && NamakaInstalled() && NamakaUpdateAvailable())
		NamakaUpdateAfterEditorUpdate();
	if(gUpdateCheckAuto)
		startCheck(false);
}

void
UpdaterCheckNow(void)
{
	if(!updatesAllowed()){
		Toast(TOAST_SELECTION, _("Updates are off in this run"));
		return;
	}
	startCheck(true);
}

static void
rollBack(void)
{
	std::string from = HaumeaFullVersion(), to = previousVersion();
	std::string error;
	if(!UpdateRollback(installDir(), &error)){
		Toast(TOAST_SELECTION, _("Couldn't roll back: %s"), error.c_str());
		return;
	}
	// Don't offer the version just left again.
	snprintf(gUpdateSkip, sizeof(gUpdateSkip), "%s", from.c_str());
	SaveEditorSettingsNow();
	writeNotice("rolledback " + from + " " + to + "\n");
	if(!RestartHaumeaKeepingArgs())
		Toast(TOAST_SELECTION, _("Rolled back to %s: restart Haumea to use it"), to.c_str());
}

// ---- GUI ----

// The release the notes are for, and its page when it has one.
static const std::string &
whatsNewVersion(void)
{
	return gNoticeKind == "updated" ? gNoticeVersion : gRelease.version;
}

static bool
whatsNewHasPage(void)
{
	return !gRelease.pageUrl.empty() && whatsNewVersion() == gRelease.version;
}

static void
whatsNewNotes(void)
{
	const std::string &notes = gNoticeKind == "updated" ? gNotes : gRelease.body;
	if(notes.empty())
		ImGui::TextDisabled(_("This release has no notes."));
	else
		ImGui::TextWrapped("%s", notes.c_str());
}

// The notes and, under them, a link to the release page (the start screen's block).
static void
whatsNewBody(void)
{
	whatsNewNotes();
	if(whatsNewHasPage())
		ImGui::TextLinkOpenURL(_("Release page"), gRelease.pageUrl.c_str());
}

// Read what a release changed: the notes filling the window, its page at the foot.
static void
whatsNewWindow(void)
{
	if(!gShowWhatsNew)
		return;
	char title[96];
	snprintf(title, sizeof(title), _("What's new in %s"), whatsNewVersion().c_str());
	snprintf(title + strlen(title), sizeof(title) - strlen(title), "###updater_whats_new");
	ImGuiWindowFlags placed = UiPlaceToolWindow(title, 30.0f, false);
	if(ImGui::Begin(title, &gShowWhatsNew, placed)){
		bool page = whatsNewHasPage();
		float foot = page ? ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y : 0.0f;
		ImGui::BeginChild("##whats_new_notes", ImVec2(0, -foot));
		whatsNewNotes();
		ImGui::EndChild();
		if(page && UiFooterButton(_("Release page")))
			OpenInFileManager(gRelease.pageUrl);
	}
	ImGui::End();
}

// What's new: its own window, or, in the start screen's inline block, a flag
// the caller reads to draw UpdaterDrawWhatsNewInline under it.
static void
whatsNewButton(const ImVec2 &size = ImVec2(0, 0))
{
	if(ImGui::Button(_("What's new"), size)){
		if(gDrawingInline)
			gWhatsNewInline = !gWhatsNewInline;
		else
			gShowWhatsNew = true;
	}
}

// The width of each of n equal buttons sharing the row.
static ImVec2
rowButton(int n)
{
	float spacing = ImGui::GetStyle().ItemSpacing.x;
	return ImVec2(floorf((ImGui::GetContentRegionAvail().x - spacing * (n - 1)) / n), 0);
}

// The one main action: a solid accent button with white text.
static bool
accentButton(const char *label, const ImVec2 &size)
{
	ImVec4 fill = UiAccentFill();
	ImGui::PushStyleColor(ImGuiCol_Button, fill);
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(fill.x*1.12f, fill.y*1.12f, fill.z*1.12f, 1.0f));
	ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(fill.x*0.9f, fill.y*0.9f, fill.z*0.9f, 1.0f));
	ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
	bool pressed = ImGui::Button(label, size);
	ImGui::PopStyleColor(4);
	return pressed;
}

static bool
bannerBegin(const char *id)
{
	ImGuiIO &io = ImGui::GetIO();
	KeepNextEditorOverlayOnMainWindow();
	ImGui::SetNextWindowPos(EditorUiPoint(io.DisplaySize.x - 10.0f, 40.0f), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
	ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 22.0f, 0));
	return ImGui::Begin(id, nil, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
		ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
		ImGuiWindowFlags_NoFocusOnAppearing);
}

// After an update or a roll back: once, until dismissed.
static void
noticeContents(void)
{
	if(gNoticeKind == "updated"){
		ImGui::Text(TI(ICON_FA_CHECK, "Updated to Haumea Studio %s"), gNoticeVersion.c_str());
		for(const std::string &p : gNoticeLocal)
			ImGui::TextWrapped(_("You had changed %s: your copy is %s.local."), p.c_str(), p.c_str());
	}else if(gNoticeKind == "rolledback"){
		ImGui::Text(TI(ICON_FA_ROTATE_LEFT, "Back to Haumea Studio %s"), gNoticeOther.c_str());
		ImGui::TextDisabled(_("%s won't be offered again."), gNoticeVersion.c_str());
	}else{
		ImGui::TextWrapped(TI(ICON_FA_TRIANGLE_EXCLAMATION, "The update wasn't installed: %s"), gNoticeError.c_str());
	}
	// What's new beside OK after an update; else OK alone, full width.
	// (Not UiFooterButton: the start screen draws this inline, mid-window.)
	ImGui::PushID("updater_notice");
	if(gNoticeKind == "updated"){
		ImGui::Spacing();
		ImVec2 half = rowButton(2);
		whatsNewButton(half);
		ImGui::SameLine();
		if(accentButton(_("OK"), half))
			gNoticeKind.clear();
	}else if(accentButton(_("OK"), ImVec2(-FLT_MIN, 0)))
		gNoticeKind.clear();
	ImGui::PopID();
}

// The banner's text and buttons, in the floating banner or the start screen.
static void
bannerContents(int status)
{
	ImGui::Text(status == UPD_DOWNLOADING ? TI(ICON_FA_BELL, "Haumea Studio %s is downloading") :
		status == UPD_READY ? TI(ICON_FA_BELL, "Haumea Studio %s is ready to install") :
		TI(ICON_FA_BELL, "Haumea Studio %s is available"), gRelease.version.c_str());
	if(status == UPD_AVAILABLE){
		if(gNotifyOnly[0])
			ImGui::TextDisabled("%s", _(gNotifyOnly));
		ImGui::Spacing();
		// What's new and Skip on top; along the foot Later, then the main
		// action at the right: Update now, or the release page when it can't.
		ImVec2 half = rowButton(2);
		whatsNewButton(half);
		ImGui::SameLine();
		if(ImGui::Button(_("Skip this version"), half)){
			snprintf(gUpdateSkip, sizeof(gUpdateSkip), "%s", gRelease.version.c_str());
			SaveEditorSettingsNow();
			gBannerHidden = true;
		}
		bool page = gNotifyOnly[0] && !gRelease.pageUrl.empty();
		ImVec2 later = !gNotifyOnly[0] || page ? half : ImVec2(-FLT_MIN, 0);
		if(ImGui::Button(_("Later"), later))
			gBannerHidden = true;
		if(!gNotifyOnly[0]){
			ImGui::SameLine();
			if(accentButton(_("Update now"), half))
				startDownload();
			ImGui::SetItemTooltip(_("Downloads it in the background; it installs when Haumea restarts."));
		}else if(page){
			ImGui::SameLine();
			if(accentButton(_("Release page"), half))
				OpenInFileManager(gRelease.pageUrl);
		}
	}else if(status == UPD_DOWNLOADING){
		const UpdateAsset *asset = UpdateBundleName() ? gRelease.Find(UpdateBundleName()) : nil;
		long long total = asset ? asset->size : 0;
		if(total > 0)
			ImGui::ProgressBar((float)((double)gDownloadedBytes / total));
		else
			ImGui::ProgressBar(-1.0f * (float)ImGui::GetTime());
		if(ImGui::Button(_("Hide"), ImVec2(-FLT_MIN, 0)))
			gBannerHidden = true;
		ImGui::SetItemTooltip(_("The download goes on; the update installs when Haumea restarts."));
	}else if(status == UPD_READY){
		const char *blocker = restartBlocker();
		ImGui::Spacing();
		// Later, then the main action at the right.
		ImVec2 half = rowButton(2);
		if(ImGui::Button(_("Later"), half))
			gBannerHidden = true;
		ImGui::SetItemTooltip(_("It installs the next time Haumea starts."));
		ImGui::SameLine();
		ImGui::BeginDisabled(blocker != nil);
		if(accentButton(_("Restart now"), half) && !RestartHaumeaKeepingArgs())
			Toast(TOAST_SELECTION, _("Couldn't restart: the update installs next time"));
		ImGui::EndDisabled();
		if(blocker)
			ImGui::SetItemTooltip(_("%s The update installs the next time Haumea starts."), blocker);
	}else{
		ImGui::PushStyleColor(ImGuiCol_Text, UiSemanticColor(UI_COLOR_ERROR));
		ImGui::TextWrapped(_("The update failed: %s"), gError.c_str());
		ImGui::PopStyleColor();
		ImGui::Spacing();
		ImVec2 half = rowButton(2);
		if(ImGui::Button(_("Dismiss"), half))
			gBannerHidden = true;
		ImGui::SameLine();
		if(accentButton(_("Try again"), half))
			startDownload();
	}
}

// The release banner is up: a newer version is available, downloading,
// ready, or failed to download.
static bool
releaseNews(int status)
{
	return !gBannerHidden && (status == UPD_AVAILABLE || status == UPD_DOWNLOADING || status == UPD_READY ||
		(status == UPD_FAILED && !gRelease.version.empty()));
}

bool
UpdaterHasNews(void)
{
	return !gNoticeKind.empty() || releaseNews(gStatus);
}

void
UpdaterDrawInline(void)
{
	ImGui::PushID("updater_inline");
	gDrawingInline = true;
	if(!gNoticeKind.empty())
		noticeContents();
	else if(releaseNews(gStatus))
		bannerContents(gStatus);
	gDrawingInline = false;
	ImGui::PopID();
}

bool
UpdaterWhatsNewRequested(void)
{
	return gWhatsNewInline && UpdaterHasNews();
}

void
UpdaterDrawWhatsNewInline(void)
{
	whatsNewBody();
}

void
UpdaterDrawGui(void)
{
	whatsNewWindow();
	int status = gStatus;
	if(gManualCheck && (status == UPD_UP_TO_DATE || status == UPD_OFFLINE || status == UPD_FAILED)){
		if(status == UPD_UP_TO_DATE)
			Toast(TOAST_SELECTION, _("Haumea Studio %s is up to date"), HaumeaFullVersion());
		else
			Toast(TOAST_SELECTION, _("Couldn't check for updates: %s"), gError.c_str());
		gManualCheck = false;
		gStatus = UPD_IDLE;
		return;
	}
	// The start screen is a modal that would dim the banner and take its
	// clicks: it draws UpdaterDrawInline() itself.
	if(StartScreenOpen())
		return;
	if(!gNoticeKind.empty()){
		if(bannerBegin("##updater_notice"))
			noticeContents();
		ImGui::End();
		return;
	}
	if(!releaseNews(status))
		return;
	if(bannerBegin("##updater"))
		bannerContents(status);
	ImGui::End();
}

void
UpdaterHelpMenuItems(void)
{
	if(ImGui::MenuItem(LI(ICON_FA_BELL, "Check for updates"), nil, false, !gWorking))
		UpdaterCheckNow();
	std::string dir = installDir();
	if(!dir.empty() && UpdateCanRollback(dir)){
		std::string prev = previousVersion();
		char label[96];
		if(prev.empty())
			snprintf(label, sizeof(label), "%s###rollback", TI(ICON_FA_ROTATE_LEFT, "Roll back to the previous version"));
		else{
			snprintf(label, sizeof(label), TI(ICON_FA_ROTATE_LEFT, "Roll back to %s"), prev.c_str());
			snprintf(label + strlen(label), sizeof(label) - strlen(label), "###rollback");
		}
		const char *blocker = restartBlocker();
		if(ImGui::MenuItem(label, nil, false, blocker == nil))
			rollBack();
		ImGui::SetItemTooltip("%s", blocker ? blocker : _("Puts back the version before the last update, then restarts."));
	}
}

// On the same line, at the row's right end when there's room, for a button.
static void
rightEnd(float right, const char *button)
{
	ImGui::SameLine();
	float w = ImGui::CalcTextSize(button, nil, true).x + ImGui::GetStyle().FramePadding.x * 2;
	if(ImGui::GetCursorPosX() < right - w)
		ImGui::SetCursorPosX(right - w);
}

void
UpdaterPreferences(void)
{
	bool changed = false;
	// The channel as a row: its name on the left, the two choices on the right.
	defaultChannel();
	int beta = followsBeta() ? 1 : 0;
	char channelId[64];
	UiFieldLabel(_("Channel"), channelId, sizeof(channelId));
	changed |= ImGui::RadioButton(_("Production"), &beta, 0);
	ImGui::SetItemTooltip(_("The tested releases everyone gets."));
	ImGui::SameLine();
	changed |= ImGui::RadioButton(_("Beta"), &beta, 1);
	ImGui::SetItemTooltip(_("New features before everyone else, and sometimes new bugs.\n"
		"You get the newest of Beta and Production."));
	snprintf(gUpdateChannel, sizeof(gUpdateChannel), "%s", beta ? "beta" : "production");
	// A CI artifact updates to the next official release; a PR build (whose
	// build number can run ahead of main's) can also switch to the latest one
	// now, even an older one.
	bool pr = strcmp(buildChannel(), "pr") == 0;
	bool backToProduction = !beta && strcmp(buildChannel(), "beta") == 0;
	ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
	if(unofficialBuild())
		ImGui::TextWrapped(_("This is %s, not an official release: it updates to the next official release."),
			pr ? _("a PR build") : _("a Dev build"));
	// A beta build back on Production is never downgraded on its own.
	if(backToProduction)
		ImGui::TextWrapped(_("You'll move to Production with the next production release newer than %s."),
			HaumeaFullVersion());
	ImGui::PopStyleColor();
	if(backToProduction || pr){
		ImGui::BeginDisabled(gWorking);
		if(ImGui::Button(pr ? _("Switch to the latest official release") : _("Go back to the latest Production now"),
			ImVec2(-FLT_MIN, 0)))
			startGoBack();
		ImGui::EndDisabled();
		ImGui::SetItemTooltip(_("Downloads the newest %s release, even if it's older than this build;\n"
			"it installs the next time Haumea starts. Your maps and settings stay."), beta ? _("Beta or Production") : _("Production"));
	}
	ImGui::Spacing();
	changed |= UiCheckbox(_("Check for updates when Haumea starts"), &gUpdateCheckAuto);
	changed |= UiCheckbox(_("Download updates automatically"), &gUpdateDownloadAuto);
	ImGui::SetItemTooltip(_("Downloads in the background; the update still installs only when Haumea restarts."));
	ImGui::Spacing();
	// The version and a skipped one as quiet lines, each action at the row's right end.
	float right = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
	if(gUpdateSkip[0]){
		ImGui::AlignTextToFramePadding();
		ImGui::TextDisabled(_("Skipping %s"), gUpdateSkip);
		rightEnd(right, _("Offer it again"));
		if(ImGui::Button(_("Offer it again"))){
			gUpdateSkip[0] = '\0';
			changed = true;
		}
	}
	ImGui::AlignTextToFramePadding();
	ImGui::TextDisabled(_("This is %s."), HaumeaVersionDisplay());
	rightEnd(right, _("Check now"));
	if(ImGui::Button(_("Check now")))
		UpdaterCheckNow();
	if(changed)
		SaveEditorSettingsNow();
}

// ---- Command line (the tests) ----

static std::string
jsonString(const std::string &s)
{
	std::string out = "\"";
	for(unsigned char c : s){
		if(c == '"' || c == '\\'){ out += '\\'; out += (char)c; }
		else if(c == '\n') out += "\\n";
		else if(c < 0x20){ char buf[8]; snprintf(buf, sizeof(buf), "\\u%04x", c); out += buf; }
		else out += (char)c;
	}
	return out + "\"";
}

static int
writeReport(const char *path, const std::string &result, const std::string &fields)
{
	std::string json = "{\"result\": " + jsonString(result) + fields + "}\n";
	if(!UpdateWriteFile(path, json)){
		fprintf(stderr, "can't write %s\n", path);
		return 2;
	}
	printf("%s", json.c_str());
	return 0;
}

//   --update-check <work dir> <report.json>
//       Check HAUMEA_UPDATE_URL (else GitHub) and, for a newer release,
//       download, verify and stage it in <work dir>/staged. The version to
//       compare with, the channel and the skipped version come from
//       HAUMEA_UPDATE_CURRENT, HAUMEA_UPDATE_CHANNEL (production or beta) and
//       HAUMEA_UPDATE_SKIP when set; HAUMEA_UPDATE_GO_BACK=1 takes the newest
//       production release even if it's older (Go back to Production). result: up_to_date, offline, staged, bad_checksum, error.
//   --update-install <staged dir> <install dir> <report.json>
//   --update-rollback <install dir> <report.json>
int
UpdaterCommandLine(int argc, char **argv)
{
	for(int i = 1; i < argc; i++){
		if(strcmp(argv[i], "--update-check") == 0 && i + 2 < argc){
			std::string work = argv[i + 1];
			const char *report = argv[i + 2];
			HaumeaVersion current = HaumeaCurrentVersion();
			if(const char *c = getenv("HAUMEA_UPDATE_CURRENT"))
				ParseHaumeaVersion(c, &current);
			const char *channel = getenv("HAUMEA_UPDATE_CHANNEL");
			const char *skip = getenv("HAUMEA_UPDATE_SKIP");
			UpdateRelease rel;
			std::string error;
			const char *goBackEnv = getenv("HAUMEA_UPDATE_GO_BACK");
			bool goBack = goBackEnv && strcmp(goBackEnv, "1") == 0;
			bool beta = channel ? strcmp(channel, "beta") == 0 : strcmp(buildChannel(), "beta") == 0;
			int status = checkReleases(work, followedChannels(beta), current, goBack || !skip ? "" : skip, goBack,
				&rel, &error);
			if(status != UPD_AVAILABLE)
				return writeReport(report, status == UPD_UP_TO_DATE ? "up_to_date" :
					status == UPD_OFFLINE ? "offline" : "error", ", \"error\": " + jsonString(error));
			bool badChecksum;
			std::string fields = ", \"version\": " + jsonString(rel.version);
			if(!downloadAndStage(work, rel, goBack, &error, &badChecksum))
				return writeReport(report, badChecksum ? "bad_checksum" : "error",
					fields + ", \"error\": " + jsonString(error));
			return writeReport(report, "staged", fields + ", \"staged\": " + jsonString(work + "/staged") +
				", \"unofficial\": " + (unofficialBuild() ? "true" : "false"));
		}
		if(strcmp(argv[i], "--update-install") == 0 && i + 3 < argc){
			UpdateInstallResult result;
			std::string error;
			if(!UpdateInstall(argv[i + 1], argv[i + 2], &result, &error))
				return writeReport(argv[i + 3], "error", ", \"error\": " + jsonString(error));
			std::string local;
			for(const std::string &p : result.keptLocal)
				local += (local.empty() ? "" : ", ") + jsonString(p);
			return writeReport(argv[i + 3], "installed", ", \"replaced\": " + std::to_string(result.replaced) +
				", \"added\": " + std::to_string(result.added) + ", \"removed\": " + std::to_string(result.removed) +
				", \"kept_local\": [" + local + "]");
		}
		if(strcmp(argv[i], "--update-rollback") == 0 && i + 2 < argc){
			std::string error;
			if(!UpdateRollback(argv[i + 1], &error))
				return writeReport(argv[i + 2], "error", ", \"error\": " + jsonString(error));
			return writeReport(argv[i + 2], "rolled_back", "");
		}
	}
	return -1;
}
