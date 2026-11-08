// SPDX-License-Identifier: GPL-2.0-or-later
#include "haumea.h"
#include "agentbridge.h"
#include "i18n.h"
#include "terminal.h"
#include "imgui_internal.h"

#include <vterm.h>

#include <algorithm>
#include <deque>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
extern char **environ;
#endif

/*
 * The shell's side: a pseudo-terminal and the process on it.
 */

namespace {

static const int MAX_SCROLLBACK = 5000;

#ifdef _WIN32
// ConPTY (Windows 10 1809 and later), looked up so the editor still starts on older ones.
typedef void *ConPty;
typedef HRESULT (WINAPI *CreatePseudoConsoleFn)(COORD, HANDLE, HANDLE, DWORD, ConPty*);
typedef HRESULT (WINAPI *ResizePseudoConsoleFn)(ConPty, COORD);
typedef void (WINAPI *ClosePseudoConsoleFn)(ConPty);
#ifndef PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE
#define PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE 0x00020016
#endif

struct Pty
{
	HANDLE input = nil, output = nil, process = nil;
	ConPty console = nil;
	int exitCode = 0;

	static HMODULE kernel(void) { return GetModuleHandleA("kernel32.dll"); }

	bool start(int cols, int rows, const char *folder, std::string *error)
	{
		auto create = (CreatePseudoConsoleFn)GetProcAddress(kernel(), "CreatePseudoConsole");
		if(create == nil){
			*error = _("The terminal needs Windows 10 version 1809 or later.");
			return false;
		}
		HANDLE ptyIn = nil, ptyOut = nil;
		if(!CreatePipe(&ptyIn, &input, nil, 0) || !CreatePipe(&output, &ptyOut, nil, 0)){
			*error = _("Could not start the shell.");
			return false;
		}
		COORD size = { (SHORT)cols, (SHORT)rows };
		HRESULT made = create(size, ptyIn, ptyOut, 0, &console);
		CloseHandle(ptyIn);
		CloseHandle(ptyOut);
		if(FAILED(made)){
			*error = _("Could not start the shell.");
			return false;
		}
		SIZE_T listSize = 0;
		InitializeProcThreadAttributeList(nil, 1, 0, &listSize);
		std::vector<char> list(listSize);
		STARTUPINFOEXW info;
		memset(&info, 0, sizeof(info));
		info.StartupInfo.cb = sizeof(info);
		info.lpAttributeList = (LPPROC_THREAD_ATTRIBUTE_LIST)list.data();
		InitializeProcThreadAttributeList(info.lpAttributeList, 1, 0, &listSize);
		UpdateProcThreadAttribute(info.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, console,
			sizeof(console), nil, nil);
		wchar_t command[MAX_PATH] = L"cmd.exe";
		GetEnvironmentVariableW(L"ComSpec", command, MAX_PATH);
		wchar_t where[MAX_PATH] = L"";
		if(folder && folder[0])
			MultiByteToWideChar(CP_UTF8, 0, folder, -1, where, MAX_PATH);
		PROCESS_INFORMATION process;
		BOOL started = CreateProcessW(nil, command, nil, nil, FALSE, EXTENDED_STARTUPINFO_PRESENT |
			CREATE_UNICODE_ENVIRONMENT, nil, where[0] ? where : nil, &info.StartupInfo, &process);
		DeleteProcThreadAttributeList(info.lpAttributeList);
		if(!started){
			*error = _("Could not start the shell.");
			return false;
		}
		CloseHandle(process.hThread);
		this->process = process.hProcess;
		return true;
	}
	int read(char *buffer, int size)	// bytes, 0 for none now, -1 at the end
	{
		DWORD ready = 0;
		if(!PeekNamedPipe(output, nil, 0, nil, &ready, nil))
			return -1;
		if(ready == 0){
			// ConPTY keeps its pipe open after the shell has gone.
			if(WaitForSingleObject(process, 0) == WAIT_OBJECT_0)
				return -1;
			return 0;
		}
		DWORD got = 0;
		if(!ReadFile(output, buffer, (DWORD)size < ready ? (DWORD)size : ready, &got, nil))
			return -1;
		return (int)got;
	}
	int write(const char *data, int size)	// bytes taken
	{
		DWORD wrote = 0;
		return WriteFile(input, data, (DWORD)size, &wrote, nil) ? (int)wrote : -1;
	}
	void resize(int cols, int rows)
	{
		auto resized = (ResizePseudoConsoleFn)GetProcAddress(kernel(), "ResizePseudoConsole");
		COORD size = { (SHORT)cols, (SHORT)rows };
		if(resized && console)
			resized(console, size);
	}
	void finish(void)
	{
		DWORD code = 0;
		if(process && GetExitCodeProcess(process, &code))
			exitCode = (int)code;
	}
	void stop(void)
	{
		auto close = (ClosePseudoConsoleFn)GetProcAddress(kernel(), "ClosePseudoConsole");
		if(console && close)
			close(console);	// ends what runs on it
		console = nil;
		for(HANDLE *h : { &input, &output, &process }){
			if(*h)
				CloseHandle(*h);
			*h = nil;
		}
	}
};
#else
struct Pty
{
	int master = -1;
	pid_t child = -1;
	int exitCode = 0;

	bool start(int cols, int rows, const char *folder, std::string *error)
	{
		*error = _("Could not start the shell.");
		master = posix_openpt(O_RDWR | O_NOCTTY);
		if(master < 0 || grantpt(master) != 0 || unlockpt(master) != 0){
			stop();
			return false;
		}
		const char *slaveName = ptsname(master);
		if(slaveName == nil){
			stop();
			return false;
		}
		std::string slavePath = slaveName;
		fcntl(master, F_SETFD, FD_CLOEXEC);
		fcntl(master, F_SETFL, fcntl(master, F_GETFL, 0) | O_NONBLOCK);
		winsize size = {};
		size.ws_col = (unsigned short)cols;
		size.ws_row = (unsigned short)rows;
		ioctl(master, TIOCSWINSZ, &size);

		// Made before fork: between fork and exec only async-signal-safe calls.
		const char *shell = getenv("SHELL");
		if(shell == nil || shell[0] != '/' || access(shell, X_OK) != 0)
			shell = "/bin/sh";
		std::string shellPath = shell;
		std::vector<std::string> variables;
		for(char **e = environ; *e; e++){
			std::string line = *e;
			std::string name = line.substr(0, line.find('='));
			if(name != "TERM" && name != "COLORTERM" && name != "TERM_PROGRAM" && name != "LINES" && name != "COLUMNS")
				variables.push_back(line);
		}
		variables.push_back("TERM=xterm-256color");
		variables.push_back("COLORTERM=truecolor");
		variables.push_back("TERM_PROGRAM=haumea");
		// A Namaka MCP server of any AI client started here finds this editor.
		const char *socketPath = AgentBridgeSocketPath();
		if(socketPath && socketPath[0]){
			variables.erase(std::remove_if(variables.begin(), variables.end(), [](const std::string &v){
				return v.compare(0, 21, "HAUMEA_ENGINE_SOCKET=") == 0;
			}), variables.end());
			variables.push_back(std::string("HAUMEA_ENGINE_SOCKET=") + socketPath);
		}
		std::vector<char*> envp;
		for(std::string &v : variables)
			envp.push_back(&v[0]);
		envp.push_back(nil);
		const char *base = strrchr(shellPath.c_str(), '/');
		std::string login = std::string("-") + (base ? base + 1 : shellPath.c_str());	// a login shell, as terminals start
		char *argv[] = { &login[0], nil };
		long maxFiles = sysconf(_SC_OPEN_MAX);
		if(maxFiles < 0 || maxFiles > 4096)
			maxFiles = 4096;
		int masterFd = master;

		child = fork();
		if(child < 0){
			stop();
			return false;
		}
		if(child == 0){
			setsid();
			int slave = open(slavePath.c_str(), O_RDWR);
			if(slave < 0)
				_exit(127);
			ioctl(slave, TIOCSCTTY, 0);
			dup2(slave, 0);
			dup2(slave, 1);
			dup2(slave, 2);
			// The editor's sockets and files aren't the shell's.
			for(int fd = 3; fd < maxFiles; fd++)
				close(fd);
			(void)masterFd;
			signal(SIGPIPE, SIG_DFL);
			signal(SIGCHLD, SIG_DFL);
			sigset_t none;
			sigemptyset(&none);
			sigprocmask(SIG_SETMASK, &none, nil);
			if(folder && folder[0] && chdir(folder) != 0)
				(void)!chdir("/");
			execve(shellPath.c_str(), argv, envp.data());
			_exit(127);
		}
		return true;
	}
	int read(char *buffer, int size)	// bytes, 0 for none now, -1 at the end
	{
		ssize_t got = ::read(master, buffer, (size_t)size);
		if(got > 0)
			return (int)got;
		if(got < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
			return 0;
		return -1;	// EIO on Linux: nobody holds the other end now
	}
	int write(const char *data, int size)	// bytes taken
	{
		ssize_t wrote = ::write(master, data, (size_t)size);
		if(wrote >= 0)
			return (int)wrote;
		return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR ? 0 : -1;
	}
	void resize(int cols, int rows)
	{
		winsize size = {};
		size.ws_col = (unsigned short)cols;
		size.ws_row = (unsigned short)rows;
		ioctl(master, TIOCSWINSZ, &size);	// and the kernel tells the program (SIGWINCH)
	}
	void finish(void)
	{
		int status = 0;
		if(child > 0 && waitpid(child, &status, 0) == child){
			exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
			child = -1;
		}
	}
	void stop(void)
	{
		if(master >= 0)
			close(master);	// a hangup for all on it
		master = -1;
		if(child > 0){
			kill(-child, SIGHUP);
			// A moment to go, then it doesn't get to outlive the editor or linger as a zombie.
			for(int i = 0; i < 20; i++){
				if(waitpid(child, nil, WNOHANG) != 0){
					child = -1;
					return;
				}
				usleep(10000);
			}
			kill(-child, SIGKILL);
			waitpid(child, nil, 0);
			child = -1;
		}
	}
};
#endif

/*
 * The screen: libvterm, and the lines scrolled off the top of it.
 */

struct Mark { int line, col; };

struct Terminal
{
	Pty pty;
	VTerm *vt = nil;
	VTermScreen *screen = nil;
	int cols = 0, rows = 0;
	bool running = false, ended = false;
	std::string failure;
	std::string pendingOutput;	// typed, and not taken by the pipe yet

	std::deque<std::vector<VTermScreenCell>> scrollback;
	int scrollUp = 0;	// lines above the live screen we are looking at
	VTermPos cursor = {0, 0};
	bool cursorVisible = true, altScreen = false;
	int mouseMode = VTERM_PROP_MOUSE_NONE;

	bool selecting = false, hasSelection = false;
	Mark selectFrom = {0, 0}, selectTo = {0, 0};
	bool mouseForwarded = false;
};
static Terminal gTerm;

static void
toShell(const char *data, size_t size, void *)
{
	gTerm.pendingOutput.append(data, size);
}

static int
onMoveCursor(VTermPos pos, VTermPos, int visible, void *)
{
	gTerm.cursor = pos;
	gTerm.cursorVisible = visible != 0;
	return 1;
}

static int
onProperty(VTermProp prop, VTermValue *value, void *)
{
	switch(prop){
	case VTERM_PROP_CURSORVISIBLE: gTerm.cursorVisible = value->boolean != 0; break;
	case VTERM_PROP_ALTSCREEN: gTerm.altScreen = value->boolean != 0; gTerm.scrollUp = 0; break;
	case VTERM_PROP_MOUSE: gTerm.mouseMode = value->number; break;
	default: break;
	}
	return 1;
}

static int
onPushLine(int cols, const VTermScreenCell *cells, void *)
{
	// Without the blanks at its end: most lines are short, and there are thousands.
	int used = cols;
	while(used > 0 && cells[used - 1].chars[0] == 0 && VTERM_COLOR_IS_DEFAULT_BG(&cells[used - 1].bg))
		used--;
	gTerm.scrollback.emplace_back(cells, cells + used);
	if((int)gTerm.scrollback.size() > MAX_SCROLLBACK){
		gTerm.scrollback.pop_front();
		gTerm.hasSelection = false;	// its lines just moved up by one
	}
	else if(gTerm.scrollUp > 0)
		gTerm.scrollUp++;	// what we are reading stays where it is
	return 1;
}

static int
onPopLine(int cols, VTermScreenCell *cells, void *)
{
	if(gTerm.scrollback.empty())
		return 0;
	const std::vector<VTermScreenCell> &line = gTerm.scrollback.back();
	VTermScreenCell blank;
	memset(&blank, 0, sizeof(blank));
	blank.width = 1;
	vterm_state_get_default_colors(vterm_obtain_state(gTerm.vt), &blank.fg, &blank.bg);
	for(int i = 0; i < cols; i++)
		cells[i] = i < (int)line.size() ? line[i] : blank;
	gTerm.scrollback.pop_back();
	return 1;
}

static void
stopTerminal(void)
{
	if(gTerm.running)
		gTerm.pty.stop();
	gTerm.running = false;
}

static void
startTerminal(int cols, int rows, const char *folder)
{
	static bool registered;
	if(!registered){
		registered = true;
		atexit(stopTerminal);	// it goes with the editor
	}
	if(gTerm.vt)
		vterm_free(gTerm.vt);
	gTerm.scrollback.clear();
	gTerm.scrollUp = 0;
	gTerm.hasSelection = false;
	gTerm.selecting = false;
	gTerm.mouseMode = VTERM_PROP_MOUSE_NONE;
	gTerm.altScreen = false;
	gTerm.cursorVisible = true;
	gTerm.pendingOutput.clear();
	gTerm.ended = false;
	gTerm.failure.clear();
	gTerm.cols = cols;
	gTerm.rows = rows;

	gTerm.vt = vterm_new(rows, cols);
	vterm_set_utf8(gTerm.vt, 1);
	vterm_output_set_callback(gTerm.vt, toShell, nil);
	VTermState *state = vterm_obtain_state(gTerm.vt);
	VTermColor foreground, background;
	vterm_color_rgb(&foreground, 222, 226, 232);
	vterm_color_rgb(&background, 15, 18, 22);
	vterm_state_set_default_colors(state, &foreground, &background);
	gTerm.screen = vterm_obtain_screen(gTerm.vt);
	static VTermScreenCallbacks callbacks;
	callbacks.movecursor = onMoveCursor;
	callbacks.settermprop = onProperty;
	callbacks.sb_pushline = onPushLine;
	callbacks.sb_popline = onPopLine;
	vterm_screen_set_callbacks(gTerm.screen, &callbacks, nil);
	vterm_screen_enable_altscreen(gTerm.screen, 1);
	vterm_screen_reset(gTerm.screen, 1);

	gTerm.pty = Pty();
	gTerm.running = gTerm.pty.start(cols, rows, folder, &gTerm.failure);
	if(gTerm.running)
		gTerm.failure.clear();
	else
		gTerm.ended = true;
}

// Every frame: what it printed, and what we typed.
static void
pumpTerminal(void)
{
	if(!gTerm.running)
		return;
	static char buffer[65536];
	// A flood of output mustn't hold the editor's frame.
	for(int i = 0; i < 16; i++){
		int got = gTerm.pty.read(buffer, sizeof(buffer));
		if(got < 0){
			gTerm.pty.finish();
			int code = gTerm.pty.exitCode;
			gTerm.pty.stop();
			gTerm.pty.exitCode = code;
			gTerm.running = false;
			gTerm.ended = true;
			break;
		}
		if(got == 0)
			break;
		vterm_input_write(gTerm.vt, buffer, (size_t)got);
	}
	vterm_screen_flush_damage(gTerm.screen);
	if(gTerm.running && !gTerm.pendingOutput.empty()){
		int wrote = gTerm.pty.write(gTerm.pendingOutput.data(), (int)gTerm.pendingOutput.size());
		if(wrote > 0)
			gTerm.pendingOutput.erase(0, (size_t)wrote);
	}
}

static VTermScreenCell
cellAt(int line, int col)
{
	VTermScreenCell cell;
	int back = (int)gTerm.scrollback.size();
	if(line < back){
		const std::vector<VTermScreenCell> &stored = gTerm.scrollback[line];
		if(col < (int)stored.size())
			return stored[col];
		memset(&cell, 0, sizeof(cell));
		cell.width = 1;
		vterm_state_get_default_colors(vterm_obtain_state(gTerm.vt), &cell.fg, &cell.bg);
		return cell;
	}
	VTermPos pos = { line - back, col };
	vterm_screen_get_cell(gTerm.screen, pos, &cell);
	return cell;
}

static void
appendUtf8(std::string &out, uint32_t c)
{
	if(c < 0x80)
		out += (char)c;
	else if(c < 0x800){
		out += (char)(0xC0 | c >> 6);
		out += (char)(0x80 | (c & 63));
	}else if(c < 0x10000){
		out += (char)(0xE0 | c >> 12);
		out += (char)(0x80 | (c >> 6 & 63));
		out += (char)(0x80 | (c & 63));
	}else{
		out += (char)(0xF0 | c >> 18);
		out += (char)(0x80 | (c >> 12 & 63));
		out += (char)(0x80 | (c >> 6 & 63));
		out += (char)(0x80 | (c & 63));
	}
}

// The text of lines from..to (as seen, wide letters once), without trailing blanks.
static std::string
textBetween(Mark from, Mark to)
{
	std::string out;
	for(int line = from.line; line <= to.line; line++){
		std::string text;
		int first = line == from.line ? from.col : 0;
		int last = line == to.line ? to.col : gTerm.cols - 1;
		for(int col = first; col <= last && col < gTerm.cols; col++){
			VTermScreenCell cell = cellAt(line, col);
			if(cell.chars[0] == (uint32_t)-1)	// the second half of a wide letter
				continue;
			if(cell.chars[0] == 0)
				text += ' ';
			for(int i = 0; i < VTERM_MAX_CHARS_PER_CELL && cell.chars[i]; i++)
				appendUtf8(text, cell.chars[i]);
		}
		while(!text.empty() && text.back() == ' ')
			text.pop_back();
		out += text;
		if(line != to.line)
			out += '\n';
	}
	return out;
}

static void
orderedSelection(Mark *from, Mark *to)
{
	*from = gTerm.selectFrom;
	*to = gTerm.selectTo;
	if(from->line > to->line || (from->line == to->line && from->col > to->col))
		std::swap(*from, *to);
}

/*
 * The panel
 */

static VTermModifier
modifiers(void)
{
	ImGuiIO &io = ImGui::GetIO();
	int mod = VTERM_MOD_NONE;
	if(io.KeyShift) mod |= VTERM_MOD_SHIFT;
	if(io.KeyAlt) mod |= VTERM_MOD_ALT;
	if(io.KeyCtrl) mod |= VTERM_MOD_CTRL;
	return (VTermModifier)mod;
}

static void
pasteClipboard(void)
{
	const char *text = ImGui::GetClipboardText();
	if(text == nil || !gTerm.running)
		return;
	// Marked as a paste for programs that asked (bracketed paste), so a pasted
	// newline isn't taken as pressing Enter.
	vterm_keyboard_start_paste(gTerm.vt);
	const unsigned char *p = (const unsigned char*)text;
	while(*p){
		unsigned int c = 0;
		int used = ImTextCharFromUtf8(&c, (const char*)p, nil);
		if(used <= 0)
			break;
		p += used;
		vterm_keyboard_unichar(gTerm.vt, c == '\n' ? '\r' : c, VTERM_MOD_NONE);
	}
	vterm_keyboard_end_paste(gTerm.vt);
	gTerm.scrollUp = 0;
}

static void
copySelection(void)
{
	if(!gTerm.hasSelection)
		return;
	Mark from, to;
	orderedSelection(&from, &to);
	ImGui::SetClipboardText(textBetween(from, to).c_str());
}

static void
handleKeyboard(void)
{
	ImGuiIO &io = ImGui::GetIO();
	VTermModifier mod = modifiers();
	static const struct { ImGuiKey key; VTermKey term; } keys[] = {
		{ ImGuiKey_Enter, VTERM_KEY_ENTER }, { ImGuiKey_KeypadEnter, VTERM_KEY_ENTER },
		{ ImGuiKey_Tab, VTERM_KEY_TAB }, { ImGuiKey_Backspace, VTERM_KEY_BACKSPACE },
		{ ImGuiKey_Escape, VTERM_KEY_ESCAPE }, { ImGuiKey_UpArrow, VTERM_KEY_UP },
		{ ImGuiKey_DownArrow, VTERM_KEY_DOWN }, { ImGuiKey_LeftArrow, VTERM_KEY_LEFT },
		{ ImGuiKey_RightArrow, VTERM_KEY_RIGHT }, { ImGuiKey_Insert, VTERM_KEY_INS },
		{ ImGuiKey_Delete, VTERM_KEY_DEL }, { ImGuiKey_Home, VTERM_KEY_HOME },
		{ ImGuiKey_End, VTERM_KEY_END }, { ImGuiKey_PageUp, VTERM_KEY_PAGEUP },
		{ ImGuiKey_PageDown, VTERM_KEY_PAGEDOWN },
	};
	bool typed = false;
	// Scrollback from the keyboard, as other terminals have it.
	if(io.KeyShift && !gTerm.altScreen && ImGui::IsKeyPressed(ImGuiKey_PageUp, true)){
		gTerm.scrollUp = std::min(gTerm.scrollUp + gTerm.rows - 1, (int)gTerm.scrollback.size());
		mod = VTERM_MOD_NONE;
	}else if(io.KeyShift && !gTerm.altScreen && ImGui::IsKeyPressed(ImGuiKey_PageDown, true)){
		gTerm.scrollUp = std::max(gTerm.scrollUp - (gTerm.rows - 1), 0);
		mod = VTERM_MOD_NONE;
	}else{
		for(const auto &k : keys)
			if(ImGui::IsKeyPressed(k.key, true)){
				vterm_keyboard_key(gTerm.vt, k.term, mod);
				typed = true;
			}
		for(int i = 0; i < 12; i++)
			if(ImGui::IsKeyPressed((ImGuiKey)(ImGuiKey_F1 + i), true)){
				vterm_keyboard_key(gTerm.vt, (VTermKey)VTERM_KEY_FUNCTION(i + 1), mod);
				typed = true;
			}
	}
	if(io.KeyCtrl && io.KeyShift){
		// Ctrl+C and Ctrl+V belong to the program, as in other terminals.
		if(ImGui::IsKeyPressed(ImGuiKey_C, false))
			copySelection();
		if(ImGui::IsKeyPressed(ImGuiKey_V, false))
			pasteClipboard();
	}else if(io.KeyCtrl){
		for(int i = 0; i < 26; i++)
			if(ImGui::IsKeyPressed((ImGuiKey)(ImGuiKey_A + i), true)){
				vterm_keyboard_unichar(gTerm.vt, 'a' + i, mod);
				typed = true;
			}
		static const struct { ImGuiKey key; char c; } symbols[] = {
			{ ImGuiKey_LeftBracket, '[' }, { ImGuiKey_RightBracket, ']' },
			{ ImGuiKey_Backslash, '\\' }, { ImGuiKey_Space, ' ' }, { ImGuiKey_Slash, '/' },
		};
		for(const auto &s : symbols)
			if(ImGui::IsKeyPressed(s.key, true)){
				vterm_keyboard_unichar(gTerm.vt, s.c, mod);
				typed = true;
			}
	}
	for(int i = 0; i < io.InputQueueCharacters.Size; i++){
		unsigned int c = io.InputQueueCharacters[i];
		if(c >= 32 && c != 127 && !io.KeyCtrl){
			vterm_keyboard_unichar(gTerm.vt, c, io.KeyAlt ? VTERM_MOD_ALT : VTERM_MOD_NONE);
			typed = true;
		}
	}
	io.InputQueueCharacters.resize(0);
	if(typed){
		gTerm.scrollUp = 0;
		gTerm.hasSelection = false;
	}
}

static void
handleMouse(ImVec2 origin, float cellWidth, float cellHeight, bool hovered)
{
	ImGuiIO &io = ImGui::GetIO();
	int col = (int)((io.MousePos.x - origin.x) / cellWidth);
	int row = (int)((io.MousePos.y - origin.y) / cellHeight);
	col = std::max(0, std::min(col, gTerm.cols - 1));
	row = std::max(0, std::min(row, gTerm.rows - 1));
	VTermModifier mod = modifiers();

	// What asked for the mouse gets it; Shift keeps it ours, as in other terminals.
	bool program = gTerm.mouseMode != VTERM_PROP_MOUSE_NONE && !io.KeyShift;
	if(program){
		if(hovered || gTerm.mouseForwarded){
			vterm_mouse_move(gTerm.vt, row, col, mod);
			static const ImGuiMouseButton buttons[] = { ImGuiMouseButton_Left, ImGuiMouseButton_Middle, ImGuiMouseButton_Right };
			for(int b = 0; b < 3; b++){
				if(hovered && ImGui::IsMouseClicked(buttons[b])){
					vterm_mouse_button(gTerm.vt, b + 1, true, mod);
					gTerm.mouseForwarded = true;
				}
				if(ImGui::IsMouseReleased(buttons[b])){
					vterm_mouse_button(gTerm.vt, b + 1, false, mod);
					gTerm.mouseForwarded = false;
				}
			}
		}
		if(hovered && io.MouseWheel != 0.0f)
			vterm_mouse_button(gTerm.vt, io.MouseWheel > 0.0f ? 4 : 5, true, mod);
		return;
	}
	if(hovered && io.MouseWheel != 0.0f){
		int lines = (int)(io.MouseWheel * 3.0f);
		if(lines == 0)
			lines = io.MouseWheel > 0.0f ? 1 : -1;
		if(gTerm.altScreen){
			// A full-screen program without the mouse scrolls with the arrows.
			for(int i = 0; i < abs(lines); i++)
				vterm_keyboard_key(gTerm.vt, lines > 0 ? VTERM_KEY_UP : VTERM_KEY_DOWN, VTERM_MOD_NONE);
		}else
			gTerm.scrollUp = std::max(0, std::min(gTerm.scrollUp + lines, (int)gTerm.scrollback.size()));
	}
	int line = (int)gTerm.scrollback.size() - gTerm.scrollUp + row;
	if(hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)){
		gTerm.selecting = true;
		gTerm.hasSelection = false;
		gTerm.selectFrom = gTerm.selectTo = { line, col };
	}
	if(gTerm.selecting && ImGui::IsMouseDown(ImGuiMouseButton_Left)){
		gTerm.selectTo = { line, col };
		if(gTerm.selectTo.line != gTerm.selectFrom.line || gTerm.selectTo.col != gTerm.selectFrom.col)
			gTerm.hasSelection = true;
	}
	if(gTerm.selecting && ImGui::IsMouseReleased(ImGuiMouseButton_Left))
		gTerm.selecting = false;
}

/*
 * What fonts don't have or can't join up: line drawing, blocks, Braille (the
 * spinners) and the marks command-line tools like. Drawn from the cell.
 */

struct CellBox { float x, y, w, h; };

// A line from the cell's centre: up, right, down, left. Weight 1 light, 2 heavy, 3 double.
static void
drawArms(ImDrawList *draw, const CellBox &b, ImU32 colour, const unsigned char *arms)
{
	float cx = floorf(b.x + b.w * .5f) + .5f, cy = floorf(b.y + b.h * .5f) + .5f;
	float right = b.x + b.w, bottom = b.y + b.h;
	float gap = std::max(1.5f, floorf(b.w * .18f));
	const ImVec2 ends[4] = { ImVec2(cx, b.y), ImVec2(right, cy), ImVec2(cx, bottom), ImVec2(b.x, cy) };
	for(int i = 0; i < 4; i++){
		if(arms[i] == 0)
			continue;
		if(arms[i] == 3){
			ImVec2 side = i % 2 == 0 ? ImVec2(gap, 0) : ImVec2(0, gap);
			draw->AddLine(ImVec2(cx - side.x, cy - side.y), ImVec2(ends[i].x - side.x, ends[i].y - side.y), colour);
			draw->AddLine(ImVec2(cx + side.x, cy + side.y), ImVec2(ends[i].x + side.x, ends[i].y + side.y), colour);
		}else
			draw->AddLine(ImVec2(cx, cy), ends[i], colour, arms[i] == 2 ? 2.0f : 1.0f);
	}
}

// False if it isn't one of ours, and a font should draw it.
static bool
drawDrawn(ImDrawList *draw, uint32_t c, const CellBox &b, ImU32 colour)
{
	static const struct { uint32_t c; unsigned char arms[4]; } lines[] = {
		{ 0x2500, {0,1,0,1} }, { 0x2501, {0,2,0,2} }, { 0x2502, {1,0,1,0} }, { 0x2503, {2,0,2,0} },
		{ 0x250C, {0,1,1,0} }, { 0x250F, {0,2,2,0} }, { 0x2510, {0,0,1,1} }, { 0x2513, {0,0,2,2} },
		{ 0x2514, {1,1,0,0} }, { 0x2517, {2,2,0,0} }, { 0x2518, {1,0,0,1} }, { 0x251B, {2,0,0,2} },
		{ 0x251C, {1,1,1,0} }, { 0x2523, {2,2,2,0} }, { 0x2524, {1,0,1,1} }, { 0x252B, {2,0,2,2} },
		{ 0x252C, {0,1,1,1} }, { 0x2533, {0,2,2,2} }, { 0x2534, {1,1,0,1} }, { 0x253B, {2,2,0,2} },
		{ 0x253C, {1,1,1,1} }, { 0x254B, {2,2,2,2} },
		{ 0x2550, {0,3,0,3} }, { 0x2551, {3,0,3,0} }, { 0x2554, {0,3,3,0} }, { 0x2557, {0,0,3,3} },
		{ 0x255A, {3,3,0,0} }, { 0x255D, {3,0,0,3} }, { 0x2560, {3,3,3,0} }, { 0x2563, {3,0,3,3} },
		{ 0x2566, {0,3,3,3} }, { 0x2569, {3,3,0,3} }, { 0x256C, {3,3,3,3} },
		{ 0x2574, {0,0,0,1} }, { 0x2575, {1,0,0,0} }, { 0x2576, {0,1,0,0} }, { 0x2577, {0,0,1,0} },
		{ 0x23BF, {1,1,0,0} },	// the bottom-left corner tools use for a result under a line
	};
	for(const auto &line : lines)
		if(line.c == c){
			drawArms(draw, b, colour, line.arms);
			return true;
		}
	float cx = floorf(b.x + b.w * .5f) + .5f, cy = floorf(b.y + b.h * .5f) + .5f;
	float right = b.x + b.w, bottom = b.y + b.h;
	switch(c){
	// Rounded corners.
	case 0x256D: draw->AddBezierQuadratic(ImVec2(cx, bottom), ImVec2(cx, cy), ImVec2(right, cy), colour, 1.0f); return true;
	case 0x256E: draw->AddBezierQuadratic(ImVec2(b.x, cy), ImVec2(cx, cy), ImVec2(cx, bottom), colour, 1.0f); return true;
	case 0x256F: draw->AddBezierQuadratic(ImVec2(cx, b.y), ImVec2(cx, cy), ImVec2(b.x, cy), colour, 1.0f); return true;
	case 0x2570: draw->AddBezierQuadratic(ImVec2(cx, b.y), ImVec2(cx, cy), ImVec2(right, cy), colour, 1.0f); return true;
	// Dashes.
	case 0x2504: case 0x2505: case 0x2508: case 0x2509: case 0x254C: case 0x254D:
		for(int i = 0; i < 3; i++)
			draw->AddLine(ImVec2(b.x + b.w * (i + .15f) / 3, cy), ImVec2(b.x + b.w * (i + .75f) / 3, cy), colour);
		return true;
	case 0x2506: case 0x2507: case 0x250A: case 0x250B: case 0x254E: case 0x254F:
		for(int i = 0; i < 3; i++)
			draw->AddLine(ImVec2(cx, b.y + b.h * (i + .15f) / 3), ImVec2(cx, b.y + b.h * (i + .75f) / 3), colour);
		return true;
	// Marks.
	case 0x2713: case 0x2714:
		draw->AddLine(ImVec2(b.x + b.w * .12f, cy), ImVec2(b.x + b.w * .4f, b.y + b.h * .72f), colour, 1.6f);
		draw->AddLine(ImVec2(b.x + b.w * .4f, b.y + b.h * .72f), ImVec2(b.x + b.w * .9f, b.y + b.h * .25f), colour, 1.6f);
		return true;
	case 0x2717: case 0x2718:
		draw->AddLine(ImVec2(b.x + b.w * .15f, b.y + b.h * .3f), ImVec2(b.x + b.w * .85f, b.y + b.h * .72f), colour, 1.6f);
		draw->AddLine(ImVec2(b.x + b.w * .85f, b.y + b.h * .3f), ImVec2(b.x + b.w * .15f, b.y + b.h * .72f), colour, 1.6f);
		return true;
	case 0x276F:	// a prompt's chevron
		draw->AddLine(ImVec2(b.x + b.w * .25f, b.y + b.h * .25f), ImVec2(b.x + b.w * .8f, cy), colour, 2.0f);
		draw->AddLine(ImVec2(b.x + b.w * .8f, cy), ImVec2(b.x + b.w * .25f, b.y + b.h * .75f), colour, 2.0f);
		return true;
	case 0x23FA:	// a record dot
		draw->AddCircleFilled(ImVec2(cx, cy), b.w * .36f, colour);
		return true;
	case 0x2722: case 0x2733: case 0x2736: case 0x273B: case 0x273D:{	// the stars of "working on it"
		float r = b.w * .46f;
		for(int i = 0; i < 3; i++){
			float a = (float)i * 3.14159265f / 3.0f + 1.5707963f;
			float dx = cosf(a) * r, dy = sinf(a) * r;
			draw->AddLine(ImVec2(cx - dx, cy - dy), ImVec2(cx + dx, cy + dy), colour, 1.4f);
		}
		return true;
	}
	}
	// Blocks.
	if(c >= 0x2580 && c <= 0x259F){
		auto fill = [&](float x0, float y0, float x1, float y1, ImU32 col){
			draw->AddRectFilled(ImVec2(b.x + b.w * x0, b.y + b.h * y0), ImVec2(b.x + b.w * x1, b.y + b.h * y1), col);
		};
		if(c == 0x2580) fill(0, 0, 1, .5f, colour);
		else if(c >= 0x2581 && c <= 0x2588) fill(0, 1.0f - (c - 0x2580) / 8.0f, 1, 1, colour);
		else if(c >= 0x2589 && c <= 0x258F) fill(0, 0, (0x2590 - c) / 8.0f, 1, colour);
		else if(c == 0x2590) fill(.5f, 0, 1, 1, colour);
		else if(c >= 0x2591 && c <= 0x2593){
			unsigned alpha = (colour >> IM_COL32_A_SHIFT & 255) * (c - 0x2590) / 4;
			fill(0, 0, 1, 1, (colour & ~IM_COL32_A_MASK) | alpha << IM_COL32_A_SHIFT);
		}
		else if(c == 0x2594) fill(0, 0, 1, .125f, colour);
		else if(c == 0x2595) fill(.875f, 0, 1, 1, colour);
		else{
			// Quadrants: which of top left, top right, bottom left, bottom right.
			static const unsigned char quads[] = { 4, 8, 1, 13, 9, 7, 11, 2, 6, 14 };	// 2596-259F
			unsigned q = quads[c - 0x2596];
			if(q & 1) fill(0, 0, .5f, .5f, colour);
			if(q & 2) fill(.5f, 0, 1, .5f, colour);
			if(q & 4) fill(0, .5f, .5f, 1, colour);
			if(q & 8) fill(.5f, .5f, 1, 1, colour);
		}
		return true;
	}
	// Braille: two columns of four dots.
	if(c >= 0x2800 && c <= 0x28FF){
		static const unsigned char dot[8][2] = { {0,0}, {0,1}, {0,2}, {1,0}, {1,1}, {1,2}, {0,3}, {1,3} };
		float r = std::max(b.w * .11f, 1.0f);
		for(int i = 0; i < 8; i++)
			if((c - 0x2800) >> i & 1)
				draw->AddCircleFilled(ImVec2(b.x + b.w * (dot[i][0] ? .72f : .28f), b.y + b.h * (dot[i][1] * 2 + 1) / 8.0f), r, colour, 8);
		return true;
	}
	return false;
}

// The terminal's face, or the interface's for what only that has (Chinese,
// when Haumea runs in it), else the missing-letter mark.
static void
drawLetter(ImDrawList *draw, const VTermScreenCell &cell, ImVec2 at, float cellWidth, int width, float size, ImU32 colour)
{
	std::string text;
	for(int i = 0; i < VTERM_MAX_CHARS_PER_CELL && cell.chars[i]; i++)
		appendUtf8(text, cell.chars[i]);
	ImFont *font = ImGui::GetFont();
	if(font->GetFontBaked(size)->FindGlyphNoFallback((ImWchar)cell.chars[0]) == nil && cell.chars[0] <= 0xFFFF){
		ImFont *ui = ImGui::GetIO().Fonts->Fonts[0];
		if(ui->GetFontBaked(size)->FindGlyphNoFallback((ImWchar)cell.chars[0])){
			float advance = ui->CalcTextSizeA(size, FLT_MAX, 0.0f, text.c_str()).x;
			draw->AddText(ui, size, ImVec2(at.x + (width * cellWidth - advance) * .5f, at.y), colour, text.c_str());
			return;
		}
	}
	draw->AddText(font, size, at, colour, text.c_str());
	if(cell.attrs.bold)	// no bold of its own at this size
		draw->AddText(font, size, ImVec2(at.x + 0.5f, at.y), colour, text.c_str());
}

static ImU32
rgbOf(VTermColor color)
{
	vterm_screen_convert_color_to_rgb(gTerm.screen, &color);
	return IM_COL32(color.rgb.red, color.rgb.green, color.rgb.blue, 255);
}

static void
drawScreen(ImDrawList *draw, ImVec2 origin, float cellWidth, float cellHeight, bool focused)
{
	float size = ImGui::GetFontSize();
	int firstLine = (int)gTerm.scrollback.size() - gTerm.scrollUp;
	Mark from = {0, 0}, to = {0, 0};
	if(gTerm.hasSelection)
		orderedSelection(&from, &to);
	for(int row = 0; row < gTerm.rows; row++){
		int line = firstLine + row;
		float y = origin.y + row * cellHeight;
		for(int col = 0; col < gTerm.cols; col++){
			VTermScreenCell cell = cellAt(line, col);
			if(cell.chars[0] == (uint32_t)-1)
				continue;	// covered by the wide letter before it
			int width = cell.width > 0 ? cell.width : 1;
			float x = origin.x + col * cellWidth;
			ImVec2 end(x + width * cellWidth, y + cellHeight);
			ImU32 foreground = rgbOf(cell.fg), background = rgbOf(cell.bg);
			bool defaultBackground = VTERM_COLOR_IS_DEFAULT_BG(&cell.bg);
			if(cell.attrs.reverse){
				std::swap(foreground, background);
				defaultBackground = false;
			}
			if(gTerm.hasSelection && (line > from.line || (line == from.line && col >= from.col)) &&
			   (line < to.line || (line == to.line && col <= to.col))){
				background = IM_COL32(70, 110, 170, 255);
				defaultBackground = false;
			}
			if(!defaultBackground)
				draw->AddRectFilled(ImVec2(x, y), end, background);
			if(cell.chars[0] && cell.chars[0] != ' ' && !cell.attrs.conceal){
				CellBox box = { x, y, width * cellWidth, cellHeight };
				if(!drawDrawn(draw, cell.chars[0], box, foreground))
					drawLetter(draw, cell, ImVec2(x, y), cellWidth, width, size, foreground);
			}
			if(cell.attrs.underline)
				draw->AddLine(ImVec2(x, end.y - 1), ImVec2(end.x, end.y - 1), foreground);
			if(cell.attrs.strike)
				draw->AddLine(ImVec2(x, y + cellHeight * .5f), ImVec2(end.x, y + cellHeight * .5f), foreground);
		}
	}
	if(gTerm.cursorVisible && gTerm.scrollUp == 0 && gTerm.running){
		VTermScreenCell under = cellAt((int)gTerm.scrollback.size() + gTerm.cursor.row, gTerm.cursor.col);
		int width = under.width > 0 ? under.width : 1;
		ImVec2 at(origin.x + gTerm.cursor.col * cellWidth, origin.y + gTerm.cursor.row * cellHeight);
		ImVec2 end(at.x + width * cellWidth, at.y + cellHeight);
		if(focused){
			draw->AddRectFilled(at, end, IM_COL32(222, 226, 232, 230));
			if(under.chars[0] && under.chars[0] != ' '){
				CellBox box = { at.x, at.y, width * cellWidth, cellHeight };
				if(!drawDrawn(draw, under.chars[0], box, IM_COL32(15, 18, 22, 255)))
					drawLetter(draw, under, at, cellWidth, width, size, IM_COL32(15, 18, 22, 255));
			}
		}else
			draw->AddRect(at, end, IM_COL32(222, 226, 232, 200));
	}
}

} // namespace

void
DrawTerminalWindow(bool *open, const char *folder)
{
	pumpTerminal();
	if(!*open)
		return;
	static bool wasOpen;
	if(!wasOpen)
		ImGui::SetNextWindowFocus();
	wasOpen = true;
	ImGui::SetNextWindowSize(ImVec2(760, 420), ImGuiCond_FirstUseEver);
	bool visible = ImGui::Begin(">_ Terminal###terminal", open,
		ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoNav);
	if(!*open)
		wasOpen = false;
	if(!visible){
		ImGui::End();
		return;
	}
	float statusHeight = gTerm.ended ? ImGui::GetFrameHeightWithSpacing() : 0.0f;
	ImGui::PushFont(gTerminalFont);
	float cellWidth = std::max(ImGui::CalcTextSize("M").x, 1.0f);
	float cellHeight = std::max(ImGui::GetTextLineHeight(), 1.0f);
	ImVec2 origin = ImGui::GetCursorScreenPos();
	ImVec2 room = ImGui::GetContentRegionAvail();
	room.y -= statusHeight;
	int cols = std::max((int)(room.x / cellWidth), 10);
	int rows = std::max((int)(room.y / cellHeight), 3);

	if(!gTerm.running && !gTerm.ended)
		startTerminal(cols, rows, folder);
	else if(gTerm.vt && (cols != gTerm.cols || rows != gTerm.rows)){
		gTerm.cols = cols;
		gTerm.rows = rows;
		vterm_set_size(gTerm.vt, rows, cols);
		vterm_screen_flush_damage(gTerm.screen);
		if(gTerm.running)
			gTerm.pty.resize(cols, rows);
		gTerm.hasSelection = false;
		gTerm.scrollUp = std::min(gTerm.scrollUp, (int)gTerm.scrollback.size());
	}

	ImGui::PushItemFlag(ImGuiItemFlags_NoTabStop, true);
	ImGui::InvisibleButton("##screen", ImVec2(std::max(room.x, 1.0f), std::max(room.y, 1.0f)));
	ImGui::PopItemFlag();
	bool hovered = ImGui::IsItemHovered();
	bool focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
	ImDrawList *draw = ImGui::GetWindowDrawList();
	draw->AddRectFilled(origin, ImVec2(origin.x + room.x, origin.y + room.y), IM_COL32(15, 18, 22, 255));
	if(gTerm.vt){
		if(gTerm.running){
			if(focused){
				// The editor's shortcuts leave it alone, and the pressed-keys
				// overlay treats it as typing: it never shows in a recording.
				ImGui::SetNextFrameWantCaptureKeyboard(true);
				ImGui::GetCurrentContext()->WantTextInputNextFrame = 1;
				handleKeyboard();
			}
			handleMouse(origin, cellWidth, cellHeight, hovered);
		}
		draw->PushClipRect(origin, ImVec2(origin.x + room.x, origin.y + room.y), true);
		drawScreen(draw, origin, cellWidth, cellHeight, focused);
		draw->PopClipRect();
	}
	ImGui::PopFont();

	if(gTerm.mouseMode == VTERM_PROP_MOUSE_NONE || ImGui::GetIO().KeyShift){
		if(ImGui::BeginPopupContextItem("##terminal menu")){
			if(ImGui::MenuItem(L("Copy"), "Ctrl+Shift+C", false, gTerm.hasSelection))
				copySelection();
			if(ImGui::MenuItem(L("Paste"), "Ctrl+Shift+V", false, gTerm.running))
				pasteClipboard();
			ImGui::EndPopup();
		}
	}
	if(gTerm.ended){
		if(ImGui::Button(L("Start again"))){
			stopTerminal();
			gTerm.ended = false;
		}
		ImGui::SameLine();
		if(!gTerm.failure.empty())
			ImGui::TextUnformatted(gTerm.failure.c_str());
		else
			ImGui::Text(_("The shell closed (exit code %d)."), gTerm.pty.exitCode);
	}
	ImGui::End();
}

std::string
TerminalScreenText(void)
{
	if(gTerm.vt == nil || gTerm.rows <= 0)
		return "";
	int top = (int)gTerm.scrollback.size();
	return textBetween({ top, 0 }, { top + gTerm.rows - 1, gTerm.cols - 1 });
}
