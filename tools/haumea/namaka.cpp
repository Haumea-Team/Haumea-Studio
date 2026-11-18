// SPDX-License-Identifier: GPL-2.0-or-later
#include "haumea.h"
#include "i18n.h"
#include "namaka.h"
#include "childprocess.h"
#include "workspace.h"
#include "agentbridge.h"
#include "icons.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <algorithm>
#include <deque>
#include <string>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#else
#include <dirent.h>
#include <unistd.h>
#endif

// Namaka's panel (Windows > Namaka): finds a Python, runs the bundle's
// namaka_install.py with it to install, repair, update or uninstall the venv
// at <config>/namaka/venv, and shows how to connect an MCP client. The
// installer is a child process, so the editor never waits on pip; its output
// goes to <config>/namaka/install.log and the panel.

// ---- Paths ----

#ifdef _WIN32
static const char *VENV_PYTHON = "Scripts/python.exe";
static const char *VENV_MCP = "Scripts/haumea-mcp.exe";
#else
static const char *VENV_PYTHON = "bin/python";
static const char *VENV_MCP = "bin/haumea-mcp";
#endif

static bool
isFile(const std::string &path)
{
	struct stat st;
	return stat(path.c_str(), &st) == 0 && (st.st_mode & S_IFMT) == S_IFREG;
}

static bool
isDir(const std::string &path)
{
	struct stat st;
	return stat(path.c_str(), &st) == 0 && (st.st_mode & S_IFMT) == S_IFDIR;
}

static std::string
namakaDir(void)
{
	return std::string(GetConfigDir()) + "/namaka";
}

// HAUMEA_NAMAKA_VENV moves it, for tests.
static std::string
venvDir(void)
{
	const char *env = getenv("HAUMEA_NAMAKA_VENV");
	return env && env[0] ? std::string(env) : namakaDir() + "/venv";
}

static std::string
logPath(void)
{
	return namakaDir() + "/install.log";
}

struct Version;
static std::string wheelVersion(const std::string &wheel);
static bool newerWheel(const std::string &a, const std::string &b);

// The bundled wheel's file name in dir, or "": the newest by version when
// there are several (the installer then asks to remove the others).
static std::string
wheelIn(const std::string &dir)
{
	std::vector<std::string> names;
#ifdef _WIN32
	WIN32_FIND_DATAA entry;
	static const char *patterns[] = { "/haumea_namaka-*.whl", "/haumea_agent-*.whl" };
	for(const char *pattern : patterns){
		HANDLE h = FindFirstFileA((dir + pattern).c_str(), &entry);
		if(h == INVALID_HANDLE_VALUE)
			continue;
		do names.push_back(entry.cFileName); while(FindNextFileA(h, &entry));
		FindClose(h);
		if(!names.empty())
			break;
	}
#else
	if(DIR *d = opendir(dir.c_str())){
		while(dirent *e = readdir(d)){
			std::string name = e->d_name;
			bool wheel = name.size() > 4 && name.compare(name.size() - 4, 4, ".whl") == 0;
			if(wheel && (name.compare(0, 14, "haumea_namaka-") == 0 || name.compare(0, 13, "haumea_agent-") == 0))
				names.push_back(name);
		}
		closedir(d);
	}
#endif
	std::string best;
	for(const std::string &name : names)
		if(best.empty() || newerWheel(name, best))
			best = name;
	return best;
}

// Where Namaka's wheel, wheelhouse and installer are: <install>/namaka in a
// release, the folder itself in a standalone Namaka bundle.
// HAUMEA_NAMAKA_BUNDLE overrides it (tests, developer builds).
static std::string
bundleDir(void)
{
	const char *env = getenv("HAUMEA_NAMAKA_BUNDLE");
	if(env && env[0])
		return env;
	char root[1024];
	if(!GetEditorRootDirectory(root, sizeof(root)))
		return "";
	std::string install = root;
	if(!wheelIn(install + "/namaka").empty())
		return install + "/namaka";
	if(!wheelIn(install).empty())
		return install;
	return "";
}

// Agent Alpha's venvs from before: still working, never updated in place.
static std::string
legacyVenv(void)
{
	std::vector<std::string> candidates;
	char root[1024];
	if(GetEditorRootDirectory(root, sizeof(root))){
		candidates.push_back(std::string(root) + "/haumea-agent-alpha/.venv-agent");
		candidates.push_back(std::string(root) + "/.venv-agent");
	}
	if(GetGameDir()[0])
		candidates.push_back(std::string(GetGameDir()) + "/haumea-agent-alpha/.venv-agent");
	for(const std::string &c : candidates)
		if(isFile(c + "/" + VENV_PYTHON))
			return c;
	return "";
}

// ---- Versions ----

// "1.0.0.100", "0.1.0a1", "1.0.0.7+dirty": the release numbers, then the
// pre-release (a < b < rc < none); a +local label doesn't count.
struct Version {
	std::vector<int> release;
	int pre = 3, preNumber = 0;	// 0 a, 1 b, 2 rc, 3 none
	bool valid = false;
};

static Version
parseVersion(const std::string &text)
{
	Version v;
	size_t i = 0;
	while(i < text.size() && isdigit((unsigned char)text[i])){
		int n = 0;
		while(i < text.size() && isdigit((unsigned char)text[i]))
			n = n * 10 + (text[i++] - '0');
		v.release.push_back(n);
		if(i < text.size() && text[i] == '.' && i + 1 < text.size() && isdigit((unsigned char)text[i+1]))
			i++;
		else
			break;
	}
	v.valid = !v.release.empty();
	static const char *pres[] = { "a", "b", "rc" };
	for(int p = 2; p >= 0; p--)
		if(text.compare(i, strlen(pres[p]), pres[p]) == 0){
			v.pre = p;
			v.preNumber = atoi(text.c_str() + i + strlen(pres[p]));
			break;
		}
	return v;
}

static bool
newer(const Version &a, const Version &b)
{
	size_t n = std::max(a.release.size(), b.release.size());
	for(size_t i = 0; i < n; i++){
		int x = i < a.release.size() ? a.release[i] : 0, y = i < b.release.size() ? b.release[i] : 0;
		if(x != y)
			return x > y;
	}
	if(a.pre != b.pre)
		return a.pre > b.pre;
	return a.preNumber > b.preNumber;
}

// haumea_namaka-<version>-py3-none-any.whl
static std::string
wheelVersion(const std::string &wheel)
{
	size_t dash = wheel.find('-');
	size_t end = dash == std::string::npos ? std::string::npos : wheel.find('-', dash + 1);
	return end == std::string::npos ? "" : wheel.substr(dash + 1, end - dash - 1);
}

static bool
newerWheel(const std::string &a, const std::string &b)
{
	return newer(parseVersion(wheelVersion(a)), parseVersion(wheelVersion(b)));
}

static std::string
installedWheel(void)
{
	FILE *f = fopen((venvDir() + "/namaka-complete").c_str(), "r");
	if(f == nil)
		return "";
	char line[256] = "";
	if(fgets(line, sizeof(line), f) == nil)
		line[0] = '\0';
	fclose(f);
	std::string name = line;
	while(!name.empty() && (name.back() == '\n' || name.back() == '\r'))
		name.pop_back();
	return name;
}

bool
NamakaInstalled(void)
{
	return isFile(venvDir() + "/" + VENV_PYTHON) && !installedWheel().empty();
}

bool
NamakaUpdateAvailable(void)
{
	std::string bundle = bundleDir();
	if(bundle.empty() || !NamakaInstalled())
		return false;
	Version have = parseVersion(wheelVersion(installedWheel()));
	Version offered = parseVersion(wheelVersion(wheelIn(bundle)));
	return offered.valid && (!have.valid || newer(offered, have));
}

// ---- Listening ----

static std::string
listenSettingPath(void)
{
	return namakaDir() + "/listen";
}

bool
NamakaListenEnabled(void)
{
	// Off only when the file says 0: on by default.
	FILE *f = fopen(listenSettingPath().c_str(), "r");
	if(f == nil)
		return true;
	int c = fgetc(f);
	fclose(f);
	return c != '0';
}

void
SetNamakaListenEnabled(bool on)
{
	EnsureParentDirectoriesForPath(listenSettingPath().c_str());
	FILE *f = fopen(listenSettingPath().c_str(), "w");
	if(f){
		fputs(on ? "1\n" : "0\n", f);
		fclose(f);
	}
}

bool
NamakaShouldListen(void)
{
	// Tests and scripts never listen on their own. HAUMEA_NAMAKA_LISTEN=1
	// lets a test check that it does (with HAUMEA_NAMAKA_SOCKET elsewhere).
	const char *forced = getenv("HAUMEA_NAMAKA_LISTEN");
	bool test = IsAutomatedRun();
	if(forced && forced[0] && !SettingsAreReadOnly())	// command-line modes never do
		test = atoi(forced) == 0;
	return !test && NamakaInstalled() && NamakaListenEnabled();
}

std::string
NamakaDefaultSocketPath(void)
{
	const char *env = getenv("HAUMEA_NAMAKA_SOCKET");
	if(env && env[0])
		return env;
#ifdef _WIN32
	return "";
#else
	// As haumea_ipc finds it: Path.home() / ".cache" / "haumea-agent.sock".
	const char *home = getenv("HOME");
	if(home == nil || home[0] == '\0')
		return "";
	std::string cache = std::string(home) + "/.cache";
	if(!isDir(cache))
		mkdir(cache.c_str(), 0700);	// an existing one is left as it is
	return cache + "/haumea-agent.sock";
#endif
}

// ---- Python ----

struct PythonCandidate {
	std::vector<std::string> command;	// e.g. { "py", "-3.14" }
	std::string version;	// "3.14", or "" when it didn't run
};

static std::vector<PythonCandidate> gPythons;	// probed, in preference order
static int gPython = -1;	// the one to use, or -1
static bool gPythonsProbed, gProbing;
static size_t gProbeNext;
static Process gProbe;
static std::string gProbeOutput;

static void
startPythonProbe(void)
{
	gPythons.clear();
	gPython = -1;
	gProbeNext = 0;
	gProbing = true;
	gPythonsProbed = false;
	// HAUMEA_NAMAKA_PYTHON names one, for tests.
	if(const char *env = getenv("HAUMEA_NAMAKA_PYTHON")){
		if(env[0]){
			gPythons.push_back({ { env }, "" });
			return;
		}
	}
#ifdef _WIN32
	static const char *versions[] = { "-3.14", "-3.13", "-3.12", "-3.11", "-3.10" };
	for(const char *v : versions)
		gPythons.push_back({ { "py", v }, "" });
	gPythons.push_back({ { "python" }, "" });
	gPythons.push_back({ { "python3" }, "" });
#else
	static const char *names[] = { "python3.14", "python3.13", "python3.12", "python3.11", "python3.10",
		"python3", "python" };
	for(const char *name : names)
		gPythons.push_back({ { name }, "" });
#endif
}

// One candidate at a time, a frame at a time: no waiting on the UI thread.
static void
updatePythonProbe(void)
{
	if(!gProbing)
		return;
	if(ProcessRunning(&gProbe)){
		if(ProcessPoll(&gProbe, [](const std::string &line){ gProbeOutput += line; }))
			return;
		PythonCandidate &c = gPythons[gProbeNext - 1];
		int major = 0, minor = 0;
		if(gProbe.exitCode == 0 && sscanf(gProbeOutput.c_str(), "%d.%d", &major, &minor) == 2)
			c.version = std::to_string(major) + "." + std::to_string(minor);
	}
	if(gProbeNext < gPythons.size()){
		gProbeOutput.clear();
		std::vector<std::string> argv = gPythons[gProbeNext++].command;
		argv.push_back("-c");
		argv.push_back("import sys; print('%d.%d' % sys.version_info[:2])");
		if(!ProcessSpawn(&gProbe, argv, ".", {}))
			gProbe = Process();	// not there: next frame tries the next one
		return;
	}
	gProbing = false;
	gPythonsProbed = true;
	// 3.14 installs offline; otherwise the newest 3.10 or later.
	int best = -1, bestMinor = -1;
	for(size_t i = 0; i < gPythons.size(); i++){
		int major = 0, minor = 0;
		if(sscanf(gPythons[i].version.c_str(), "%d.%d", &major, &minor) != 2 || major != 3 || minor < 10)
			continue;
		int score = minor == 14 ? 1000 : minor;
		if(score > bestMinor){
			best = (int)i;
			bestMinor = score;
		}
	}
	gPython = best;
}

static std::string
pythonLabel(void)
{
	if(gPython < 0)
		return "";
	std::string text;
	for(const std::string &part : gPythons[gPython].command)
		text += (text.empty() ? "" : " ") + part;
	return "Python " + gPythons[gPython].version + " (" + text + ")";
}

static bool
pythonIsOffline(void)
{
	return gPython >= 0 && gPythons[gPython].version == "3.14";
}

// What to install, per system, when no usable Python is found.
static const char*
pythonAdvice(void)
{
#ifdef _WIN32
	return _("Install Python 3.14 from python.org (tick \"Add python.exe to PATH\"), or run\n"
		"winget install Python.Python.3.14, then press Look again.");
#elif defined(__APPLE__)
	return _("Install Python 3.14 from python.org, or run brew install python@3.14,\n"
		"then press Look again.");
#else
	return _("Install Python 3.14 (or 3.10 or newer) with its venv module, then press Look again:\n"
		"Debian/Ubuntu: sudo apt install python3 python3-venv\n"
		"Fedora: sudo dnf install python3\n"
		"Arch: sudo pacman -S python");
#endif
}

// ---- The installer job ----

enum JobKind { JOB_NONE, JOB_INSTALL, JOB_REPAIR, JOB_UPDATE, JOB_UNINSTALL };
static JobKind gJob = JOB_NONE, gQueued = JOB_NONE;
static Process gJobProcess;
static FILE *gLog;
static std::string gStep, gResult;
static bool gResultOk;
static int gStepNumber, gStepCount;
static std::deque<std::string> gLines;	// the last lines, for the panel
static int gTools = -1;
static bool gNoticeAfterJob;	// the updater's run: say how it went in a toast

static const char*
jobVerb(JobKind job)
{
	switch(job){
	case JOB_INSTALL: return "install";
	case JOB_REPAIR: return "repair";
	case JOB_UPDATE: return "update";
	case JOB_UNINSTALL: return "uninstall";
	default: return "";
	}
}

static void
jobLine(const std::string &line)
{
	if(gLog){
		fprintf(gLog, "%s\n", line.c_str());
		fflush(gLog);
	}
	gLines.push_back(line);
	while(gLines.size() > 12)
		gLines.pop_front();
	int n, m;
	if(sscanf(line.c_str(), "STEP %d/%d", &n, &m) == 2){
		gStepNumber = n;
		gStepCount = m;
		size_t space = line.find(' ', 5);
		gStep = space == std::string::npos ? line : line.substr(space + 1);
	}else if(line.compare(0, 5, "DONE ") == 0 || line == "DONE"){
		gResult = line.size() > 5 ? line.substr(5) : "Done.";
		gResultOk = true;
	}else if(line.compare(0, 5, "FAIL ") == 0){
		gResult = line.substr(5);
		gResultOk = false;
	}else if(line.compare(0, 6, "TOOLS ") == 0)
		gTools = atoi(line.c_str() + 6);
}

static bool
startJob(JobKind job, std::string *error)
{
	if(gJob != JOB_NONE){
		*error = "Namaka's installer is already running";
		return false;
	}
	std::string bundle = bundleDir();
	std::string installer = bundle.empty() ? "" : bundle + "/namaka_install.py";
	std::vector<std::string> argv;
	if(job == JOB_UNINSTALL && NamakaInstalled()){
		// Needs no other Python, nor the bundle when the venv has a copy.
		argv.push_back(venvDir() + "/" + VENV_PYTHON);
		if(!isFile(installer))
			installer = venvDir() + "/namaka_install.py";
	}else if(gPython >= 0)
		argv = gPythons[gPython].command;
	if(argv.empty()){
		*error = "no Python 3.10 or newer was found";
		return false;
	}
	if(!isFile(installer)){
		*error = "this Haumea has no Namaka bundle (namaka_install.py)";
		return false;
	}
	argv.push_back(installer);
	argv.push_back(job == JOB_INSTALL ? "install" : job == JOB_REPAIR ? "repair" :
		job == JOB_UPDATE ? "update" : "uninstall");
	if(!bundle.empty()){
		argv.push_back("--bundle");
		argv.push_back(bundle);
	}
	argv.push_back("--venv");
	argv.push_back(venvDir());
	EnsureParentDirectoriesForPath(logPath().c_str());
	gLog = fopen(logPath().c_str(), "w");
	gLines.clear();
	gStep = std::string("Starting the ") + jobVerb(job);
	gStepNumber = 0;
	gStepCount = 0;
	gResult.clear();
	if(!ProcessSpawn(&gJobProcess, argv, namakaDir(), { "PYTHONUNBUFFERED=1" })){
		if(gLog){ fclose(gLog); gLog = nil; }
		*error = "could not start " + argv[0];
		return false;
	}
	jobLine("$ " + argv[0] + " " + installer + " " + jobVerb(job));
	gJob = job;
	return true;
}

static void
updateJob(void)
{
	if(gJob == JOB_NONE){
		// The updater's run, once the one before it is done.
		if(gQueued != JOB_NONE && gPythonsProbed){
			JobKind next = gQueued;
			gQueued = JOB_NONE;
			std::string error;
			if(!startJob(next, &error))
				ToastFor(TOAST_SAVE, 8.0f, _("Namaka was not updated: %s"), error.c_str());
		}
		return;
	}
	if(ProcessPoll(&gJobProcess, jobLine))
		return;
	if(gResult.empty()){
		gResultOk = gJobProcess.exitCode == 0;
		char stopped[96];
		snprintf(stopped, sizeof(stopped), _("The installer stopped (exit code %d)."), gJobProcess.exitCode);
		gResult = gResultOk ? _("Done.") : stopped;
	}
	if(gLog){
		fclose(gLog);
		gLog = nil;
	}
	// A copy of the installer in the venv, so Uninstall works without the bundle.
	if(gResultOk && gJob != JOB_UNINSTALL && isFile(bundleDir() + "/namaka_install.py")){
		FILE *in = fopen((bundleDir() + "/namaka_install.py").c_str(), "rb");
		FILE *out = in ? fopen((venvDir() + "/namaka_install.py").c_str(), "wb") : nil;
		char buffer[8192];
		size_t n;
		while(in && out && (n = fread(buffer, 1, sizeof(buffer), in)) > 0)
			fwrite(buffer, 1, n, out);
		if(in) fclose(in);
		if(out) fclose(out);
	}
	if(gNoticeAfterJob){
		gNoticeAfterJob = false;
		ToastFor(TOAST_SAVE, 10.0f, gResultOk ? _("Namaka updated: %s") : _("Namaka was not updated: %s"), gResult.c_str());
	}
	gJob = JOB_NONE;
}

static void
cancelJob(void)
{
	if(gJob == JOB_NONE)
		return;
	ProcessKill(&gJobProcess, jobLine);
	jobLine("FAIL Cancelled. The next install or repair puts the previous Namaka back if it was moved aside.");
	if(gLog){
		fclose(gLog);
		gLog = nil;
	}
	gJob = JOB_NONE;
}

void
NamakaUpdateAfterEditorUpdate(void)
{
	// Queued: it runs once Python is found and anything running is done.
	if(!gPythonsProbed && !gProbing)
		startPythonProbe();
	gQueued = JOB_UPDATE;
	gNoticeAfterJob = true;
}

// The log, in the system's text viewer.
static void
openExternally(const std::string &path)
{
#ifdef _WIN32
	ShellExecuteA(nil, "open", path.c_str(), nil, nil, SW_SHOWNORMAL);
#else
#ifdef __APPLE__
	std::string command = "open " + ProcessQuote(path) + " >/dev/null 2>&1 &";
#else
	std::string command = "xdg-open " + ProcessQuote(path) + " >/dev/null 2>&1 &";
#endif
	if(system(command.c_str()) != 0)
		log("Namaka: could not open %s\n", path.c_str());
#endif
}

// ---- Connecting ----

static std::string
jsonString(const std::string &text)
{
	std::string out = "\"";
	for(char c : text){
		if(c == '"' || c == '\\')
			out += '\\';
		out += c;
	}
	return out + "\"";
}

// How an MCP client reaches this editor: the socket it listens on, or the
// default one the launchers use; on Windows the TCP port and token.
static std::vector<std::pair<std::string, std::string>>
connectionEnv(void)
{
	std::vector<std::pair<std::string, std::string>> env;
	const char *port = getenv("HAUMEA_ENGINE_TCP_PORT");
	const char *token = getenv("HAUMEA_ENGINE_TOKEN");
	if(port && port[0] && token && token[0]){
		env.push_back({ "HAUMEA_ENGINE_TCP_PORT", port });
		env.push_back({ "HAUMEA_ENGINE_TOKEN", token });
		return env;
	}
	// The default socket is haumea_ipc's default too: no env needed. Only a
	// socket given with --agent-socket has to be named.
	std::string socket = AgentBridgeSocketPath();
	if(!socket.empty() && socket != NamakaDefaultSocketPath())
		env.push_back({ "HAUMEA_ENGINE_SOCKET", socket });
	return env;
}

static std::string
mcpCommand(void)
{
	return venvDir() + "/" + VENV_MCP;
}

static std::string
claudeCodeConfig(void)
{
	std::string text = "claude mcp add haumea";
	for(const auto &e : connectionEnv())
		text += " -e " + ProcessQuote(e.first + "=" + e.second);
	return text + " -- " + ProcessQuote(mcpCommand());
}

static std::string
codexConfig(void)
{
	std::string text = "codex mcp add haumea";
	for(const auto &e : connectionEnv())
		text += " --env " + ProcessQuote(e.first + "=" + e.second);
	return text + " -- " + ProcessQuote(mcpCommand());
}

// A TOML basic string: Windows paths keep their backslashes.
static std::string
tomlString(const std::string &text)
{
	std::string out = "\"";
	for(char c : text){
		if(c == '\\' || c == '"')
			out += '\\';
		out += c;
	}
	return out + "\"";
}

// The same server in ~/.codex/config.toml.
static std::string
codexToml(void)
{
	std::string text = "[mcp_servers.haumea]\ncommand = " + tomlString(mcpCommand()) + "\n";
	std::vector<std::pair<std::string, std::string>> env = connectionEnv();
	if(!env.empty()){
		text += "\n[mcp_servers.haumea.env]\n";
		for(const auto &e : env)
			text += e.first + " = " + tomlString(e.second) + "\n";
	}
	return text;
}

// The mcpServers entry Claude Desktop and most other MCP clients read.
static std::string
mcpServersJson(void)
{
	std::string text = "{\n  \"mcpServers\": {\n    \"haumea\": {\n      \"command\": " + jsonString(mcpCommand());
	std::vector<std::pair<std::string, std::string>> env = connectionEnv();
	if(!env.empty()){
		text += ",\n      \"env\": {";
		for(size_t i = 0; i < env.size(); i++)
			text += std::string(i ? "," : "") + "\n        " + jsonString(env[i].first) + ": " + jsonString(env[i].second);
		text += "\n      }";
	}
	return text + "\n    }\n  }\n}\n";
}

// ---- The panel ----

// A line of text in a theme colour, wrapping at the panel's edge.
static void
colouredLine(int color, const char *text)
{
	ImGui::PushStyleColor(ImGuiCol_Text, UiSemanticColor(color));
	ImGui::TextWrapped("%s", text);
	ImGui::PopStyleColor();
}

static void
dimLine(const char *text)
{
	ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
	ImGui::TextWrapped("%s", text);
	ImGui::PopStyleColor();
}

// One coloured line saying where Namaka stands, then its facts as rows and
// what stands in the way of installing it.
static void
statusSection(void)
{
	std::string bundle = bundleDir();
	bool installed = NamakaInstalled();
	if(installed){
		char text[256];
		snprintf(text, sizeof(text), TI(ICON_FA_CHECK, "Namaka %s is installed"), wheelVersion(installedWheel()).c_str());
		ImGui::TextColored(UiSemanticColor(UI_COLOR_SUCCESS), "%s", text);
	}else
		ImGui::TextUnformatted(_("Namaka is not installed."));
	ImGui::SetItemTooltip("%s", _("Namaka lets an AI assistant (Claude Code, Codex, Claude Desktop or another MCP client) "
		"look at and edit the map open in Haumea."));
	if(installed && gTools >= 0){
		ImGui::SameLine();
		ImGui::TextDisabled(_("(%d tools)"), gTools);
	}
	ImGui::Spacing();
	if(installed)
		UiInfoRow(_("Folder"), venvDir().c_str());
	if(gProbing)
		UiInfoRow(_("Python"), _("Looking for Python..."));
	else if(gPython >= 0)
		UiInfoRow(_("Python"), pythonLabel().substr(7).c_str(), pythonIsOffline() ?
			_("Installs offline from the bundle.") :
			_("Downloads the parts the bundle has only for Python 3.14 (needs internet)."));
	if(!bundle.empty())
		UiInfoRow(_("Bundled"), wheelVersion(wheelIn(bundle)).c_str(), bundle.c_str());

	// What stands in the way, in its colour.
	if(bundle.empty())
		colouredLine(UI_COLOR_WARNING,
			_("This Haumea has no Namaka bundle (a developer build): use a release, or set HAUMEA_NAMAKA_BUNDLE."));
	if(!gProbing && gPython < 0){
		colouredLine(UI_COLOR_ERROR, _("No Python 3.10 or newer found."));
		dimLine(pythonAdvice());
	}else if(!gProbing && !installed && !pythonIsOffline())
		colouredLine(UI_COLOR_WARNING, _("Downloads the parts the bundle has only for Python 3.14 (needs internet)."));
	std::string legacy = legacyVenv();
	if(!legacy.empty()){
		colouredLine(UI_COLOR_WARNING, _("Agent Alpha is also installed (it still works):"));
		ImGui::SetItemTooltip("%s\n\n%s", legacy.c_str(),
			_("Install Namaka, point your MCP client at it (Copy below), then delete this folder."));
	}
}

// The installer's progress or how it went, then Repair and Uninstall, with
// Look again and the log as buttons at the row's end.
static void
maintenanceSection(void)
{
	bool busy = gJob != JOB_NONE;
	bool installed = NamakaInstalled();
	bool canRun = !busy && !gProbing && gPython >= 0 && !bundleDir().empty();
	if(busy){
		float progress = gStepCount > 0 ? (gStepNumber - 0.5f) / gStepCount : 0.0f;
		ImGui::ProgressBar(std::max(0.02f, progress), ImVec2(-FLT_MIN, 0), gStep.c_str());
		if(!gLines.empty())
			UiElidedText(gLines.back().substr(0, 120).c_str(), ImGui::GetContentRegionAvail().x);
	}else if(!gResult.empty())
		colouredLine(gResultOk ? UI_COLOR_SUCCESS : UI_COLOR_ERROR, gResult.c_str());

	ImGuiStyle &style = ImGui::GetStyle();
	float h = ImGui::GetFrameHeight();
	bool hasLog = isFile(logPath());
	int icons = (gProbing ? 0 : 1) + (hasLog ? 1 : 0);
	float avail = ImGui::GetContentRegionAvail().x;
	float iconsWidth = icons * (h + style.ItemSpacing.x);
	bool first = true;
	std::string error;
	if(installed){
		ImVec2 half(floorf((avail - iconsWidth - style.ItemSpacing.x) / 2.0f), 0);
		ImGui::BeginDisabled(!canRun);
		if(ImGui::Button(LI(ICON_FA_WRENCH, "Repair"), half) && !startJob(JOB_REPAIR, &error))
			ToastFor(TOAST_SAVE, 6.0f, "Namaka: %s", error.c_str());
		ImGui::EndDisabled();
		ImGui::SetItemTooltip(_("Installs it again from the bundle; the old install stays until the new one works."));
		ImGui::SameLine();
		ImGui::BeginDisabled(busy);
		if(ImGui::Button(LI(ICON_FA_TRASH, "Uninstall"), half) && !startJob(JOB_UNINSTALL, &error))
			ToastFor(TOAST_SAVE, 6.0f, "Namaka: %s", error.c_str());
		ImGui::EndDisabled();
		first = false;
	}else if(icons > 0){
		// Nothing else on the row: the icons go to its right end.
		ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail - iconsWidth + style.ItemSpacing.x);
	}
	ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
	if(!gProbing){
		if(!first)
			ImGui::SameLine();
		first = false;
		if(ImGui::Button(ICON_FA_MAGNIFYING_GLASS "###Look again", ImVec2(h, h)))
			startPythonProbe();
		ImGui::SetItemTooltip("%s", _("Look again"));
	}
	if(hasLog){
		if(!first)
			ImGui::SameLine();
		if(ImGui::Button(ICON_FA_LIST "###Open the log", ImVec2(h, h)))
			openExternally(logPath());
		ImGui::SetItemTooltip("%s\n%s", _("Open the log"), logPath().c_str());
	}
	ImGui::PopStyleColor();
}

// "Let Namaka connect", in the panel and Preferences.
void
NamakaListenSwitch(void)
{
	bool on = NamakaListenEnabled();
	std::string label = std::string(_("Let Namaka connect")) + "###namaka_listen";
	if(UiCheckbox(label.c_str(), &on))
		SetNamakaListenEnabled(on);
	ImGui::SetItemTooltip(_("When Namaka is installed, Haumea listens for it on its own socket\n"
		"(only your user can use it). Changes apply from the next start."));
}

// Whether Haumea is listening for Namaka, then the commands that connect an
// assistant to it, two to a row.
static void
connectSection(void)
{
	UiHeading(_("Connect an AI assistant"));
	NamakaListenSwitch();
	char text[512];
	if(AgentBridgeSocketPath()[0]){
		snprintf(text, sizeof(text), TI(ICON_FA_CHECK, "Listening for Namaka on %s"), AgentBridgeSocketPath());
		colouredLine(UI_COLOR_SUCCESS, text);
	}else if(AgentBridgeListening())	// loopback TCP (Windows)
		colouredLine(UI_COLOR_SUCCESS, TI(ICON_FA_CHECK, "Listening for Namaka"));
	else if(AgentBridgeSocketBusy())
		colouredLine(UI_COLOR_WARNING,
			_("Another Haumea is using the Namaka socket, so this one isn't listening. "
			"Close the other one and start this again to connect here."));
	else if(!NamakaInstalled())
		dimLine(_("Once Namaka is installed, Haumea listens for it from its next start."));
	else if(!NamakaListenEnabled())
		dimLine(_("Not listening: \"Let Namaka connect\" is off."));
	else
		dimLine(_("Haumea starts listening for Namaka from its next start."));
	ImGui::Spacing();
	float avail = ImGui::GetContentRegionAvail().x;
	ImVec2 half(floorf((avail - ImGui::GetStyle().ItemSpacing.x) / 2.0f), 0);
	ImGui::BeginDisabled(!NamakaInstalled());
	if(ImGui::Button(LI(ICON_FA_COPY, "Copy for Claude Code"), half)){
		ImGui::SetClipboardText(claudeCodeConfig().c_str());
		Toast(TOAST_COPY_PASTE, _("Copied the claude mcp add command"));
	}
	ImGui::SetItemTooltip("%s", claudeCodeConfig().c_str());
	ImGui::SameLine();
	if(ImGui::Button(LI(ICON_FA_COPY, "Copy for Codex"), half)){
		ImGui::SetClipboardText(codexConfig().c_str());
		Toast(TOAST_COPY_PASTE, _("Copied the codex mcp add command"));
	}
	ImGui::SetItemTooltip(_("%s\n\nOr in ~/.codex/config.toml:\n%s"), codexConfig().c_str(), codexToml().c_str());
	if(ImGui::Button(LI(ICON_FA_COPY, "Copy for Claude Desktop"), half)){
		ImGui::SetClipboardText(mcpServersJson().c_str());
		Toast(TOAST_COPY_PASTE, _("Copied the claude_desktop_config.json entry"));
	}
	ImGui::SetItemTooltip("%s", mcpServersJson().c_str());
	ImGui::SameLine();
	if(ImGui::Button(LI(ICON_FA_COPY, "Copy MCP JSON"), half)){
		ImGui::SetClipboardText(mcpServersJson().c_str());
		Toast(TOAST_COPY_PASTE, _("Copied the mcpServers entry"));
	}
	ImGui::SetItemTooltip(_("For any other MCP client: add this to its mcpServers.\n%s"), mcpServersJson().c_str());
	ImGui::EndDisabled();
}

// Let an AI assistant work on the open map: install Namaka once (the foot's
// button), then copy the command that connects your assistant to Haumea;
// update, repair or remove it when needed.
void
DrawNamakaWindow(bool *open)
{
	// The updater's queued run goes on without the panel.
	updatePythonProbe();
	updateJob();
	if(open == nil || !*open)
		return;
	if(!gPythonsProbed && !gProbing)
		startPythonProbe();
	const char *title = ICON_FA_BOLT " Namaka###namaka";
	ImGuiWindowFlags flags = UiPlaceToolWindow(title, 30.0f, true);
	if(!ImGui::Begin(title, open, flags)){
		ImGui::End();
		return;
	}
	UiFitItemsToPanel();
	bool installed = NamakaInstalled();
	statusSection();
	// Installed, connecting is what the window is for; until then, installing.
	if(installed){
		connectSection();
		UiHeading(_("Install and update"));
	}else{
		ImGui::Spacing();
		NamakaListenSwitch();
	}
	maintenanceSection();

	// The main action: Cancel while the installer runs, else Install or Update.
	bool busy = gJob != JOB_NONE;
	bool canRun = !busy && !gProbing && gPython >= 0 && !bundleDir().empty();
	std::string error;
	if(busy){
		if(UiFooterButton(LI(ICON_FA_XMARK, "Cancel")))
			cancelJob();
	}else if(!installed){
		if(UiFooterButton(LI(ICON_FA_PLUS, "Install"), canRun) && !startJob(JOB_INSTALL, &error))
			ToastFor(TOAST_SAVE, 6.0f, "Namaka: %s", error.c_str());
	}else if(NamakaUpdateAvailable()){
		if(UiFooterButton(LI(ICON_FA_ROTATE, "Update"), canRun) && !startJob(JOB_UPDATE, &error))
			ToastFor(TOAST_SAVE, 6.0f, "Namaka: %s", error.c_str());
	}
	ImGui::End();
}
