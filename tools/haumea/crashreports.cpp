// SPDX-License-Identifier: GPL-2.0-or-later
#include "haumea.h"
#include "i18n.h"
#include "version.h"
#include "versionutil.h"
#include "crashreports.h"
#include "telemetry.h"
#include "workspace.h"
#include "startscreen.h"

#include <algorithm>
#include <atomic>
#include <map>
#include <string>
#include <vector>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#ifdef _WIN32
#include <process.h>
#include <wincrypt.h>
#else
#include <dirent.h>
#include <execinfo.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <sys/utsname.h>
#ifdef __APPLE__
#include <sys/ucontext.h>	// <ucontext.h> wants _XOPEN_SOURCE there; only the type is used
#else
#include <ucontext.h>
#endif
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <dlfcn.h>
#else
#include <link.h>
#endif
#endif

// See crashreports.h. Files, in <config>/reports:
//   running-<pid>.raw    opened at start, written only by the crash handler
//   pending/<id>.json    reports waiting to be sent (exactly what is sent)
//   settings.txt         "send ask|always|never"

static const char *DEFAULT_URL = "https://reports.itsneufox.xyz/haumea/reports";
static const char *PRIVACY_URL = "https://telemetry.itsneufox.xyz/privacy";
enum { MAX_FRAMES = 64, LOG_LINES = 200, LOG_LINE = 300, MAX_REPORT = 256 * 1024,
	MAX_ERRORS_PER_SESSION = 3, KEEP_DAYS = 30 };
enum { SEND_ASK, SEND_ALWAYS, SEND_NEVER };

static std::string gReportsDir;
static std::string gRawPath;
static bool gInstalled;

// ---- What the handler writes, prepared beforehand ----

static char gHeader[2048];	// "key\tvalue\n" lines: version, commit, ... build_id
static size_t gHeaderLen;
static char gBuildId[80];
static char gGlRenderer[200] = "", gGlVersion[200] = "";
static char gLog[LOG_LINES][LOG_LINE];
static std::atomic<unsigned> gLogNext(0);
static std::atomic_flag gLogBusy = ATOMIC_FLAG_INIT;
static char gMode[16] = "", gWindow[120] = "";

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
archName(void)
{
#if defined(__aarch64__) || defined(_M_ARM64)
	return "arm64";
#else
	return "x86_64";
#endif
}

static std::string
osVersion(void)
{
#ifdef _WIN32
	std::string text = "Windows";
	typedef LONG (WINAPI *RtlGetVersionFn)(OSVERSIONINFOW*);
	HMODULE ntdll = GetModuleHandleA("ntdll.dll");
	RtlGetVersionFn get = ntdll ? (RtlGetVersionFn)(void*)GetProcAddress(ntdll, "RtlGetVersion") : nil;
	OSVERSIONINFOW info = {};
	info.dwOSVersionInfoSize = sizeof(info);
	if(get && get(&info) == 0)
		text += " " + std::to_string(info.dwMajorVersion) + "." + std::to_string(info.dwMinorVersion) +
			"." + std::to_string(info.dwBuildNumber);
	typedef const char *(CDECL *WineVersionFn)(void);
	WineVersionFn wine = ntdll ? (WineVersionFn)(void*)GetProcAddress(ntdll, "wine_get_version") : nil;
	if(wine)
		text += std::string(" (Wine ") + wine() + ")";
	return text;
#else
	struct utsname u;
	std::string kernel = uname(&u) == 0 ? std::string(u.sysname) + " " + u.release : std::string("unknown");
#ifdef __linux__
	if(FILE *f = fopen("/etc/os-release", "r")){
		char line[256];
		while(fgets(line, sizeof(line), f))
			if(strncmp(line, "PRETTY_NAME=", 12) == 0){
				std::string name = line + 12;
				name.erase(std::remove(name.begin(), name.end(), '"'), name.end());
				name.erase(std::remove(name.begin(), name.end(), '\n'), name.end());
				fclose(f);
				return name + " (" + kernel + ")";
			}
		fclose(f);
	}
#endif
	return kernel;
#endif
}

// The build's id, for its symbols: GNU build-id (Linux), the PDB's GUID and
// age (Windows, as symbol servers name it), the Mach-O UUID (macOS).
static void
findBuildId(void)
{
	gBuildId[0] = '\0';
#if defined(_WIN32)
	HMODULE self = GetModuleHandleA(nil);
	IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER*)self;
	if(dos == nil || dos->e_magic != IMAGE_DOS_SIGNATURE)
		return;
	IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS*)((char*)self + dos->e_lfanew);
	IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
	IMAGE_DEBUG_DIRECTORY *entries = (IMAGE_DEBUG_DIRECTORY*)((char*)self + dir.VirtualAddress);
	for(DWORD i = 0; dir.VirtualAddress && i < dir.Size / sizeof(IMAGE_DEBUG_DIRECTORY); i++){
		if(entries[i].Type != IMAGE_DEBUG_TYPE_CODEVIEW)
			continue;
		const unsigned char *cv = (const unsigned char*)self + entries[i].AddressOfRawData;
		if(memcmp(cv, "RSDS", 4) != 0)
			continue;
		GUID guid;
		DWORD age;
		memcpy(&guid, cv + 4, sizeof(guid));
		memcpy(&age, cv + 20, sizeof(age));
		snprintf(gBuildId, sizeof(gBuildId), "%08lX%04X%04X%02X%02X%02X%02X%02X%02X%02X%02X%lX",
			(unsigned long)guid.Data1, guid.Data2, guid.Data3, guid.Data4[0], guid.Data4[1], guid.Data4[2],
			guid.Data4[3], guid.Data4[4], guid.Data4[5], guid.Data4[6], guid.Data4[7], (unsigned long)age);
		return;
	}
#elif defined(__APPLE__)
	const struct mach_header_64 *header = (const struct mach_header_64*)_dyld_get_image_header(0);
	if(header == nil || header->magic != MH_MAGIC_64)
		return;
	const struct load_command *cmd = (const struct load_command*)(header + 1);
	for(uint32_t i = 0; i < header->ncmds; i++){
		if(cmd->cmd == LC_UUID){
			const uint8_t *u = ((const struct uuid_command*)cmd)->uuid;
			for(int k = 0; k < 16; k++)
				snprintf(gBuildId + k * 2, 3, "%02X", u[k]);
			return;
		}
		cmd = (const struct load_command*)((const char*)cmd + cmd->cmdsize);
	}
#else
	dl_iterate_phdr([](struct dl_phdr_info *info, size_t, void*) -> int {
		if(info->dlpi_name && info->dlpi_name[0])
			return 0;	// the main program has no name here
		for(int i = 0; i < info->dlpi_phnum; i++){
			const ElfW(Phdr) &ph = info->dlpi_phdr[i];
			if(ph.p_type != PT_NOTE)
				continue;
			const char *p = (const char*)(info->dlpi_addr + ph.p_vaddr), *end = p + ph.p_memsz;
			while(p + sizeof(ElfW(Nhdr)) <= end){
				const ElfW(Nhdr) *note = (const ElfW(Nhdr)*)p;
				const char *name = p + sizeof(ElfW(Nhdr));
				const unsigned char *desc = (const unsigned char*)name + ((note->n_namesz + 3) & ~3u);
				if(note->n_type == NT_GNU_BUILD_ID && note->n_namesz == 4 && memcmp(name, "GNU", 4) == 0){
					for(unsigned k = 0; k < note->n_descsz && k < 39; k++)
						snprintf(gBuildId + k * 2, 3, "%02x", desc[k]);
					return 1;
				}
				p = (const char*)desc + ((note->n_descsz + 3) & ~3u);
			}
		}
		return 1;
	}, nil);
#endif
}

static void
appendHeader(const char *key, const std::string &value)
{
	std::string clean = value;
	std::replace(clean.begin(), clean.end(), '\n', ' ');
	std::replace(clean.begin(), clean.end(), '\t', ' ');
	int n = snprintf(gHeader + gHeaderLen, sizeof(gHeader) - gHeaderLen, "%s\t%s\n", key, clean.c_str());
	if(n > 0 && gHeaderLen + n < sizeof(gHeader))
		gHeaderLen += n;
}

static void
buildHeader(void)
{
	gHeaderLen = 0;
	appendHeader("version", HaumeaFullVersion());
	appendHeader("commit", HaumeaCommit());
	appendHeader("channel", HAUMEA_CHANNEL);
	appendHeader("os", osName());
	appendHeader("os_version", osVersion());
	appendHeader("arch", archName());
	appendHeader("gl_renderer", gGlRenderer);
	appendHeader("gl_version", gGlVersion);
	appendHeader("build_id", gBuildId);
}

// ---- Modules: where each stack address belongs ----

struct Module
{
	char name[64];
	uintptr_t lo, hi;	// where it is loaded
	uintptr_t bias;	// offsets count from here: the ELF addresses the symbols use
};
static Module gModules[160];
static int gNumModules;

static const char*
baseName(const char *path)
{
	const char *slash = strrchr(path, '/');
	const char *back = strrchr(path, '\\');
	const char *p = slash > back ? slash : back;
	return p ? p + 1 : path;
}

static void
scanModules(void)
{
#if defined(_WIN32) || defined(__APPLE__)
	gNumModules = 0;	// looked up when needed (GetModuleHandleEx, dladdr)
#else
	gNumModules = 0;
	dl_iterate_phdr([](struct dl_phdr_info *info, size_t, void*) -> int {
		if(gNumModules >= (int)(sizeof(gModules) / sizeof(gModules[0])))
			return 1;
		uintptr_t lo = UINTPTR_MAX, hi = 0;
		for(int i = 0; i < info->dlpi_phnum; i++){
			const ElfW(Phdr) &ph = info->dlpi_phdr[i];
			if(ph.p_type != PT_LOAD)
				continue;
			lo = std::min(lo, (uintptr_t)(info->dlpi_addr + ph.p_vaddr));
			hi = std::max(hi, (uintptr_t)(info->dlpi_addr + ph.p_vaddr + ph.p_memsz));
		}
		if(hi <= lo)
			return 0;
		Module &m = gModules[gNumModules++];
		const char *name = info->dlpi_name && info->dlpi_name[0] ? baseName(info->dlpi_name) : "haumea";
		snprintf(m.name, sizeof(m.name), "%s", name);
		m.lo = lo;
		m.hi = hi;
		m.bias = info->dlpi_addr;
		return 0;
	}, nil);
#endif
}

// The module and offset of a code address; async-signal-safe on Linux.
static bool
moduleOf(uintptr_t pc, const char **name, uintptr_t *offset)
{
#if defined(_WIN32)
	static char found[MAX_PATH];
	HMODULE module = nil;
	if(!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
	                       (LPCSTR)pc, &module) || module == nil)
		return false;
	char path[MAX_PATH];
	DWORD n = GetModuleFileNameA(module, path, sizeof(path));
	if(n == 0 || n >= sizeof(path))
		return false;
	snprintf(found, sizeof(found), "%s", baseName(path));
	for(char *c = found; *c; c++)
		if(*c >= 'A' && *c <= 'Z') *c += 'a' - 'A';	// haumea.exe however it was named
	*name = found;
	*offset = pc - (uintptr_t)module;
	return true;
#elif defined(__APPLE__)
	Dl_info info;
	if(!dladdr((void*)pc, &info) || info.dli_fbase == nil)
		return false;
	*name = info.dli_fname && strstr(info.dli_fname, "/haumea") && !strstr(info.dli_fname, ".dylib") ?
		"haumea" : baseName(info.dli_fname ? info.dli_fname : "?");
	*offset = pc - (uintptr_t)info.dli_fbase;
	return true;
#else
	for(int i = 0; i < gNumModules; i++)
		if(pc >= gModules[i].lo && pc < gModules[i].hi){
			*name = gModules[i].name;
			*offset = pc - gModules[i].bias;
			return true;
		}
	return false;
#endif
}

// ---- Async-signal-safe output ----

#ifdef _WIN32
static HANDLE gRawFile = INVALID_HANDLE_VALUE;
static void
rawWrite(const char *s, size_t n)
{
	DWORD done;
	if(gRawFile != INVALID_HANDLE_VALUE)
		WriteFile(gRawFile, s, (DWORD)n, &done, nil);
}
#else
static int gRawFd = -1;
static void
rawWrite(const char *s, size_t n)
{
	while(gRawFd >= 0 && n > 0){
		ssize_t w = write(gRawFd, s, n);
		if(w <= 0)
			return;
		s += w;
		n -= (size_t)w;
	}
}
#endif

static void
rawText(const char *s)
{
	rawWrite(s, strlen(s));
}

static void
rawHex(uint64_t v)
{
	char buf[19];
	int i = 18;
	buf[i] = '\0';
	do{
		buf[--i] = "0123456789abcdef"[v & 15];
		v >>= 4;
	}while(v && i > 2);
	buf[--i] = 'x';
	buf[--i] = '0';
	rawText(buf + i);
}

static void
rawDecimal(uint64_t v)
{
	char buf[24];
	int i = 23;
	buf[i] = '\0';
	do{
		buf[--i] = (char)('0' + v % 10);
		v /= 10;
	}while(v && i > 0);
	rawText(buf + i);
}

// One line of text without tabs or newlines.
static void
rawField(const char *key, const char *value)
{
	rawText(key);
	rawText("\t");
	for(const char *p = value; *p; p++){
		char c = *p == '\n' || *p == '\t' || *p == '\r' ? ' ' : *p;
		rawWrite(&c, 1);
	}
	rawText("\n");
}

static void
rawFrame(uintptr_t pc)
{
	const char *name;
	uintptr_t offset;
	rawText("frame\t");
	if(moduleOf(pc, &name, &offset)){
		rawText(name);
		rawText("\t");
		rawHex(offset);
	}else{
		rawText("?\t");
		rawHex(pc);
	}
	rawText("\n");
}

// Everything but the stack and the reason.
static void
rawCommon(void)
{
	rawWrite(gHeader, gHeaderLen);
	rawText("time\t");
	rawDecimal((uint64_t)time(nil));
	rawText("\n");
	rawField("mode", gMode);
	rawField("window", gWindow);
	unsigned next = gLogNext.load();
	unsigned first = next > LOG_LINES ? next - LOG_LINES : 0;
	for(unsigned i = first; i < next; i++)
		rawField("log", gLog[i % LOG_LINES]);
}

#ifndef _WIN32
static const char*
signalName(int sig)
{
	switch(sig){
	case SIGSEGV: return "SIGSEGV";
	case SIGABRT: return "SIGABRT";
	case SIGBUS: return "SIGBUS";
	case SIGFPE: return "SIGFPE";
	case SIGILL: return "SIGILL";
	}
	return "SIGSEGV";
}

static uintptr_t
faultPc(void *context)
{
	ucontext_t *uc = (ucontext_t*)context;
	(void)uc;
#if defined(__APPLE__) && defined(__aarch64__)
	return (uintptr_t)uc->uc_mcontext->__ss.__pc;
#elif defined(__APPLE__)
	return (uintptr_t)uc->uc_mcontext->__ss.__rip;
#elif defined(__x86_64__)
	return (uintptr_t)uc->uc_mcontext.gregs[REG_RIP];
#elif defined(__aarch64__)
	return (uintptr_t)uc->uc_mcontext.pc;
#else
	return 0;
#endif
}

static volatile sig_atomic_t gInHandler;
static const int gSignals[] = { SIGSEGV, SIGABRT, SIGBUS, SIGFPE, SIGILL };

static void
onSignal(int sig, siginfo_t *info, void *context)
{
	if(gInHandler)
		_exit(128 + sig);
	gInHandler = 1;
	rawField("signal", signalName(sig));
	rawText("address\t");
	rawHex((uint64_t)(uintptr_t)(info ? info->si_addr : nil));
	rawText("\n");
	// backtrace() from here starts with this handler and the kernel's return
	// trampoline; the stack proper starts at the faulting address.
	void *pcs[MAX_FRAMES];
	int n = backtrace(pcs, MAX_FRAMES);
	uintptr_t pc = faultPc(context);
	int start = -1;
	for(int i = 0; i < n && start < 0; i++)
		if((uintptr_t)pcs[i] == pc)
			start = i;
	if(start < 0){
		if(pc)
			rawFrame(pc);
		start = std::min(n, 2);
	}
	for(int i = start; i < n; i++)
		rawFrame((uintptr_t)pcs[i]);
	rawCommon();
	rawText("end\n");
	if(gRawFd >= 0)
		fsync(gRawFd);
	signal(sig, SIG_DFL);
	raise(sig);
}
#else
static volatile LONG gInHandler;

static LONG WINAPI
onException(EXCEPTION_POINTERS *ep)
{
	if(InterlockedExchange(&gInHandler, 1))
		return EXCEPTION_CONTINUE_SEARCH;
	char code[16];
	snprintf(code, sizeof(code), "0x%08lX", (unsigned long)ep->ExceptionRecord->ExceptionCode);
	rawField("code", code);
	rawText("address\t");
	rawHex((uint64_t)(uintptr_t)ep->ExceptionRecord->ExceptionAddress);
	rawText("\n");
#if defined(_M_X64) || defined(__x86_64__)
	CONTEXT ctx = *ep->ContextRecord;
	for(int i = 0; i < MAX_FRAMES && ctx.Rip; i++){
		rawFrame((uintptr_t)ctx.Rip);
		DWORD64 imageBase = 0;
		PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx.Rip, &imageBase, nil);
		if(fn == nil){
			// A leaf function: the return address is on top of the stack.
			ctx.Rip = *(DWORD64*)ctx.Rsp;
			ctx.Rsp += 8;
		}else{
			PVOID handlerData;
			DWORD64 establisher;
			RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, ctx.Rip, fn, &ctx, &handlerData, &establisher, nil);
		}
	}
#else
	rawFrame((uintptr_t)ep->ExceptionRecord->ExceptionAddress);
#endif
	rawCommon();
	rawText("end\n");
	FlushFileBuffers(gRawFile);
	return EXCEPTION_CONTINUE_SEARCH;
}
#endif

// ---- Small helpers ----

static std::string
joinDir(const std::string &a, const char *b)
{
	return a + "/" + b;
}

static void
makeDir(const std::string &path)
{
#ifdef _WIN32
	CreateDirectoryA(path.c_str(), nil);
#else
	mkdir(path.c_str(), 0700);
#endif
}

static std::vector<std::string>
listDir(const std::string &dir, const char *suffix)
{
	std::vector<std::string> names;
#ifdef _WIN32
	WIN32_FIND_DATAA fd;
	HANDLE h = FindFirstFileA((dir + "/*").c_str(), &fd);
	if(h != INVALID_HANDLE_VALUE){
		do{
			std::string n = fd.cFileName;
			if(n.size() > strlen(suffix) && n.compare(n.size() - strlen(suffix), std::string::npos, suffix) == 0)
				names.push_back(n);
		}while(FindNextFileA(h, &fd));
		FindClose(h);
	}
#else
	if(DIR *d = opendir(dir.c_str())){
		while(struct dirent *e = readdir(d)){
			std::string n = e->d_name;
			if(n.size() > strlen(suffix) && n.compare(n.size() - strlen(suffix), std::string::npos, suffix) == 0)
				names.push_back(n);
		}
		closedir(d);
	}
#endif
	std::sort(names.begin(), names.end());
	return names;
}

static bool
readFile(const std::string &path, std::string *out)
{
	FILE *f = fopen(path.c_str(), "rb");
	if(f == nil)
		return false;
	out->clear();
	char buf[8192];
	size_t n;
	while((n = fread(buf, 1, sizeof(buf), f)) > 0)
		out->append(buf, n);
	fclose(f);
	return true;
}

static bool
writeFile(const std::string &path, const std::string &text)
{
	std::string temporary = path + ".tmp";
	FILE *f = fopen(temporary.c_str(), "wb");
	if(f == nil)
		return false;
	bool ok = fwrite(text.data(), 1, text.size(), f) == text.size();
	ok = fclose(f) == 0 && ok;
	if(!ok){
		remove(temporary.c_str());
		return false;
	}
#ifdef _WIN32
	return MoveFileExA(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != FALSE;
#else
	return rename(temporary.c_str(), path.c_str()) == 0;
#endif
}

static bool
randomHex(std::string *out)
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
	char hex[33];
	for(int i = 0; i < 16; i++)
		snprintf(hex + i * 2, 3, "%02x", ok ? bytes[i] : (unsigned)(rand() & 0xFF));
	*out = hex;
	return true;
}

static std::string
isoTime(time_t when)
{
	struct tm t;
#ifdef _WIN32
	gmtime_s(&t, &when);
#else
	gmtime_r(&when, &t);
#endif
	char text[32];
	strftime(text, sizeof(text), "%Y-%m-%dT%H:%M:%SZ", &t);
	return text;
}

static std::string
jsonString(const std::string &s)
{
	std::string out = "\"";
	for(unsigned char c : s){
		if(c == '"' || c == '\\'){
			out += '\\';
			out += (char)c;
		}else if(c < 0x20){
			char esc[8];
			snprintf(esc, sizeof(esc), "\\u%04x", c);
			out += esc;
		}else
			out += (char)c;
	}
	return out + "\"";
}

// SHA-1, for the signature the server recomputes.
static std::string
sha1Hex(const std::string &message)
{
	uint32_t h[5] = { 0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0 };
	std::string m = message;
	uint64_t bits = (uint64_t)message.size() * 8;
	m += (char)0x80;
	while(m.size() % 64 != 56)
		m += (char)0;
	for(int i = 7; i >= 0; i--)
		m += (char)(bits >> (i * 8));
	for(size_t chunk = 0; chunk < m.size(); chunk += 64){
		uint32_t w[80];
		for(int i = 0; i < 16; i++)
			w[i] = (uint32_t)(unsigned char)m[chunk + i * 4] << 24 | (uint32_t)(unsigned char)m[chunk + i * 4 + 1] << 16 |
				(uint32_t)(unsigned char)m[chunk + i * 4 + 2] << 8 | (uint32_t)(unsigned char)m[chunk + i * 4 + 3];
		for(int i = 16; i < 80; i++){
			uint32_t v = w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16];
			w[i] = v << 1 | v >> 31;
		}
		uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
		for(int i = 0; i < 80; i++){
			uint32_t f, k;
			if(i < 20){ f = (b & c) | (~b & d); k = 0x5A827999; }
			else if(i < 40){ f = b ^ c ^ d; k = 0x6ED9EBA1; }
			else if(i < 60){ f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
			else{ f = b ^ c ^ d; k = 0xCA62C1D6; }
			uint32_t t = (a << 5 | a >> 27) + f + e + k + w[i];
			e = d; d = c; c = b << 30 | b >> 2; b = a; a = t;
		}
		h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
	}
	char out[41];
	for(int i = 0; i < 5; i++)
		snprintf(out + i * 8, 9, "%08x", h[i]);
	return out;
}

// ---- Scrubbing: nothing that names the user, their folders or their maps ----

static void
replaceAll(std::string &s, const std::string &from, const std::string &to)
{
	if(from.size() < 3)
		return;
	for(size_t at = s.find(from); at != std::string::npos; at = s.find(from, at + to.size()))
		s.replace(at, from.size(), to);
}

static std::string
scrub(std::string s)
{
	std::vector<std::pair<std::string, std::string>> swaps;
	auto add = [&](const char *value, const char *placeholder){
		if(value == nil || strlen(value) < 3)
			return;
		std::string v = value;
		while(v.size() > 1 && (v.back() == '/' || v.back() == '\\'))
			v.pop_back();
		swaps.push_back({ v, placeholder });
		std::string back = v, forward = v;
		std::replace(back.begin(), back.end(), '/', '\\');
		std::replace(forward.begin(), forward.end(), '\\', '/');
		swaps.push_back({ back, placeholder });
		swaps.push_back({ forward, placeholder });
	};
	add(GetWorkspaceDir(), "<WORKSPACE>");
	add(GetGameDir(), "<GTA>");
	add(GetConfigDir(), "<CONFIG>");
#ifdef _WIN32
	add(getenv("USERPROFILE"), "<HOME>");
	add(getenv("USERNAME"), "<USER>");
#else
	add(getenv("HOME"), "<HOME>");
	add(getenv("USER"), "<USER>");
#endif
	// Longest first: the workspace is usually inside the home folder.
	std::stable_sort(swaps.begin(), swaps.end(), [](const std::pair<std::string, std::string> &a,
	                                               const std::pair<std::string, std::string> &b){
		return a.first.size() > b.first.size();
	});
	for(auto &swap : swaps)
		replaceAll(s, swap.first, swap.second);
	// Map files by name: <MAP>.pwn and the like.
	static const char *extensions[] = { ".pwn", ".inc", ".map", ".ipl", ".amx" };
	for(const char *ext : extensions){
		size_t len = strlen(ext);
		for(size_t at = 0; (at = s.find(ext, at)) != std::string::npos; at += len){
			size_t start = at;
			while(start > 0 && !strchr(" \t\"'/\\:<>()[]=,", s[start - 1]))
				start--;
			if(start < at && s.compare(start, 5, "<MAP>") != 0){
				s.replace(start, at - start, "<MAP>");
				at = start + 5;
			}
		}
	}
	return s;
}

// ---- Reports ----

struct Frame
{
	std::string module, offset;
};

static std::string
signatureOf(const std::string &kind, const std::string &reason, const std::vector<Frame> &frames)
{
	std::string text = kind + "\n" + reason + "\n";
	int own = 0;
	for(const Frame &f : frames)
		if((f.module == "haumea" || f.module == "haumea.exe") && own++ < 5)
			text += f.module + "+" + f.offset + "\n";
	return sha1Hex(text);
}

struct ReportFields
{
	std::map<std::string, std::string> header;
	std::string kind, reason, address, message, where;
	int count = 1;
	std::vector<Frame> frames;
	std::vector<std::string> log;
	std::string mode, window;
	time_t created;
};

static std::string
reportJson(const ReportFields &r, std::string *id)
{
	auto field = [&](const char *key) -> std::string {
		auto it = r.header.find(key);
		return it == r.header.end() ? std::string() : it->second;
	};
	randomHex(id);
	std::string reason = r.kind == "crash" ? r.reason : scrub(r.where);
	std::string json = "{\"schema\":1,\"id\":" + jsonString(*id) + ",\"kind\":" + jsonString(r.kind) +
		",\"created\":" + jsonString(isoTime(r.created)) +
		",\"version\":" + jsonString(field("version")) + ",\"commit\":" + jsonString(field("commit")) +
		",\"channel\":" + jsonString(field("channel")) + ",\"os\":" + jsonString(field("os")) +
		",\"os_version\":" + jsonString(field("os_version").substr(0, 120)) +
		",\"arch\":" + jsonString(field("arch")) +
		",\"gl_renderer\":" + jsonString(field("gl_renderer").substr(0, 200)) +
		",\"gl_version\":" + jsonString(field("gl_version").substr(0, 200)) +
		",\"signature\":" + jsonString(signatureOf(r.kind, reason, r.frames));
	if(!field("build_id").empty())
		json += ",\"build_id\":" + jsonString(field("build_id"));
	if(r.kind == "crash"){
		bool isSignal = r.reason.compare(0, 3, "SIG") == 0;
		json += std::string(",\"crash\":{") + (isSignal ? "\"signal\":" : "\"code\":") + jsonString(r.reason) +
			",\"address\":" + jsonString(r.address.empty() ? "0x0" : r.address) + "}";
	}else
		json += ",\"error\":{\"message\":" + jsonString(scrub(r.message).substr(0, 1000)) + ",\"where\":" +
			jsonString(reason.substr(0, 200)) + ",\"count\":" + std::to_string(r.count) + "}";
	json += ",\"frames\":[";
	for(size_t i = 0; i < r.frames.size() && i < MAX_FRAMES; i++)
		json += std::string(i ? "," : "") + "{\"module\":" + jsonString(r.frames[i].module.substr(0, 128)) +
			",\"offset\":" + jsonString(r.frames[i].offset) + "}";
	json += "],\"log\":[";
	size_t first = r.log.size() > LOG_LINES ? r.log.size() - LOG_LINES : 0;
	for(size_t i = first; i < r.log.size(); i++)
		json += std::string(i > first ? "," : "") + jsonString(scrub(r.log[i]).substr(0, 300));
	json += "],\"context\":{\"mode\":" + jsonString(r.mode.substr(0, 16)) + ",\"window\":" +
		jsonString(scrub(r.window).substr(0, 120)) + "}}";
	return json;
}

static std::string
pendingDir(void)
{
	return joinDir(gReportsDir, "pending");
}

static bool
savePending(const std::string &json, const std::string &id)
{
	if(json.size() > MAX_REPORT - 1024)
		return false;	// never happens with 200 lines of 300 characters
	makeDir(pendingDir());
	return writeFile(joinDir(pendingDir(), (id + ".json").c_str()), json);
}

// A crash file from a run that ended (not one still running) into a report.
static void
convertRaw(const std::string &path)
{
	std::string text;
	if(!readFile(path, &text) || text.find("\nend\n") == std::string::npos){
		remove(path.c_str());	// a clean exit, or killed before it could write
		return;
	}
	ReportFields r;
	r.kind = "crash";
	r.created = time(nil);
	size_t at = 0;
	while(at < text.size()){
		size_t eol = text.find('\n', at);
		if(eol == std::string::npos)
			eol = text.size();
		std::string line = text.substr(at, eol - at);
		at = eol + 1;
		size_t tab = line.find('\t');
		std::string key = tab == std::string::npos ? line : line.substr(0, tab);
		std::string value = tab == std::string::npos ? std::string() : line.substr(tab + 1);
		if(key == "signal" || key == "code") r.reason = value;
		else if(key == "address") r.address = value;
		else if(key == "time") r.created = (time_t)atoll(value.c_str());
		else if(key == "mode") r.mode = value;
		else if(key == "window") r.window = value;
		else if(key == "log") r.log.push_back(value);
		else if(key == "frame"){
			size_t t = value.find('\t');
			if(t != std::string::npos)
				r.frames.push_back({ value.substr(0, t), value.substr(t + 1) });
		}else if(key != "end")
			r.header[key] = value;
	}
	if(r.reason.empty() || r.frames.empty()){
		remove(path.c_str());
		return;
	}
	std::string id;
	std::string json = reportJson(r, &id);
	if(savePending(json, id)){
		log("Crash reports: last run crashed (%s); report %s is waiting\n", r.reason.c_str(), id.c_str());
		remove(path.c_str());
	}
}

static bool
processAlive(long pid)
{
#ifdef _WIN32
	HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, (DWORD)pid);
	if(h == nil)
		return false;
	bool alive = WaitForSingleObject(h, 0) == WAIT_TIMEOUT;
	CloseHandle(h);
	return alive;
#else
	return kill((pid_t)pid, 0) == 0;
#endif
}

static long
currentPid(void)
{
#ifdef _WIN32
	return (long)GetCurrentProcessId();
#else
	return (long)getpid();
#endif
}

// ---- Settings, pending reports and sending ----

static int gSendMode = SEND_ASK;
static bool gSettingsLoaded;
static std::vector<std::string> gPending;	// file names in pending/
static bool gBannerDismissed;
static bool gShowViewer;
static std::atomic<bool> gSending(false);
static std::vector<std::string> gSendPaths, gSendBodies;
static std::vector<int> gSendStatus;
static std::atomic<bool> gSendDone(false);
static std::string gSendUrl;

static void
loadSettings(void)
{
	if(gSettingsLoaded)
		return;
	gSettingsLoaded = true;
	std::string text;
	if(readFile(joinDir(gReportsDir, "settings.txt"), &text)){
		if(text.find("send always") != std::string::npos) gSendMode = SEND_ALWAYS;
		else if(text.find("send never") != std::string::npos) gSendMode = SEND_NEVER;
	}
}

static void
saveSettings(void)
{
	if(SettingsAreReadOnly())
		return;
	makeDir(gReportsDir);
	writeFile(joinDir(gReportsDir, "settings.txt"), std::string("send ") +
		(gSendMode == SEND_ALWAYS ? "always" : gSendMode == SEND_NEVER ? "never" : "ask") + "\n");
}

static void
refreshPending(void)
{
	gPending = listDir(pendingDir(), ".json");
}

static void
deletePending(void)
{
	for(const std::string &name : gPending)
		remove(joinDir(pendingDir(), name.c_str()).c_str());
	gPending.clear();
}

static const char*
testUrl(void)
{
	const char *url = getenv("HAUMEA_REPORTS_URL");
	return url && url[0] ? url : nil;
}

static bool
hasArgument(const char *name)
{
	for(int i = 1; i < sk::args.argc; i++)
		if(strcmp(sk::args.argv[i], name) == 0 || strncmp(sk::args.argv[i], name, strlen(name)) == 0)
			return true;
	return false;
}

// Why nothing may be asked or sent from this run, or nil.
static const char*
blockedBecause(void)
{
	const char *off = getenv("HAUMEA_NO_REPORTS");
	if(off && off[0])
		return "HAUMEA_NO_REPORTS";
	if(testUrl())
		return nil;
	if(IsAutomatedRun())
		return "an automated run";
	if(hasArgument("--agent-socket"))
		return "an agent session";
	if(getenv("CI") || getenv("GITHUB_ACTIONS"))
		return "CI";
	return nil;
}

static void
sendWorker(void)
{
	for(size_t i = 0; i < gSendBodies.size(); i++)
		gSendStatus[i] = TelemetryPostJson(gSendUrl, gSendBodies[i]);
	gSendDone = true;
}

#ifdef _WIN32
static unsigned __stdcall sendThread(void*) { sendWorker(); return 0; }
#else
static void *sendThread(void*) { sendWorker(); return nil; }
#endif

static void
sendPending(void)
{
	if(gSending || gPending.empty() || blockedBecause())
		return;
	std::string url = testUrl() ? testUrl() : DEFAULT_URL;
	if(!TelemetryUrlAllowed(url, testUrl() != nil)){
		log("Crash reports: refusing %s (https only)\n", url.c_str());
		return;
	}
	std::string uid = TelemetryInstallId();	// only while the daily ping is on
	gSendPaths.clear();
	gSendBodies.clear();
	for(const std::string &name : gPending){
		std::string path = joinDir(pendingDir(), name.c_str()), body;
		if(!readFile(path, &body) || body.size() < 2 || body.size() > MAX_REPORT)
			continue;
		if(!uid.empty())
			body.insert(body.size() - 1, ",\"uid\":" + jsonString(uid));
		gSendPaths.push_back(path);
		gSendBodies.push_back(body);
	}
	if(gSendBodies.empty())
		return;
	gSendStatus.assign(gSendBodies.size(), 0);
	gSendUrl = url;
	gSendDone = false;
	gSending = true;
#ifdef _WIN32
	HANDLE h = (HANDLE)_beginthreadex(nil, 0, sendThread, nil, 0, nil);
	if(h) CloseHandle(h); else gSending = false;
#else
	pthread_t t;
	if(pthread_create(&t, nil, sendThread, nil) == 0) pthread_detach(t); else gSending = false;
#endif
}

void
CrashReportsUpdate(void)
{
	if(!gSending || !gSendDone)
		return;
	int sent = 0;
	for(size_t i = 0; i < gSendPaths.size(); i++){
		if(gSendStatus[i] >= 200 && gSendStatus[i] < 300){
			remove(gSendPaths[i].c_str());
			sent++;
		}else
			log("Crash reports: a report wasn't taken (%s); it is kept for next time\n",
				gSendStatus[i] ? ("HTTP " + std::to_string(gSendStatus[i])).c_str() : "no answer");
	}
	if(sent)
		log("Crash reports: sent %d report(s)\n", sent);
	gSending = false;
	refreshPending();
}

// ---- Public ----

void
CrashReportsInstall(void)
{
	if(gInstalled)
		return;
	gInstalled = true;
	gReportsDir = joinDir(GetConfigDir(), "reports");
	findBuildId();
	scanModules();
	buildHeader();
#ifndef _WIN32
	{
		void *warm[2];
		backtrace(warm, 2);	// loads what backtrace needs now, not in the handler
	}
#endif
	if(SettingsAreReadOnly() && !getenv("HAUMEA_CRASH_TEST"))
		return;	// command-line modes keep nothing
	makeDir(gReportsDir);
	gRawPath = joinDir(gReportsDir, ("running-" + std::to_string(currentPid()) + ".raw").c_str());
#ifdef _WIN32
	gRawFile = CreateFileA(gRawPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nil, CREATE_ALWAYS,
		FILE_ATTRIBUTE_NORMAL, nil);
	if(gRawFile == INVALID_HANDLE_VALUE)
		return;
	SetUnhandledExceptionFilter(onException);
#else
	gRawFd = open(gRawPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
	if(gRawFd < 0)
		return;
	// Its own stack, so a stack overflow can still be reported.
	static char altStack[64 * 1024];
	stack_t ss = {};
	ss.ss_sp = altStack;
	ss.ss_size = sizeof(altStack);
	sigaltstack(&ss, nil);
	struct sigaction sa = {};
	sa.sa_sigaction = onSignal;
	sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESETHAND;
	sigemptyset(&sa.sa_mask);
	for(int sig : gSignals)
		sigaction(sig, &sa, nil);
#endif
	atexit(CrashReportsShutdown);
}

void
CrashReportsNoteGraphics(void)
{
#ifdef RW_GL3
	const char *renderer = (const char*)glGetString(GL_RENDERER);
	const char *version = (const char*)glGetString(GL_VERSION);
	snprintf(gGlRenderer, sizeof(gGlRenderer), "%s", renderer ? renderer : "");
	snprintf(gGlVersion, sizeof(gGlVersion), "%s", version ? version : "");
#elif defined(RW_D3D9)
	snprintf(gGlRenderer, sizeof(gGlRenderer), "Direct3D 9");
#endif
	scanModules();	// the graphics driver's libraries came in since
	buildHeader();
}

void
CrashReportsShutdown(void)
{
#ifdef _WIN32
	if(gRawFile != INVALID_HANDLE_VALUE){
		CloseHandle(gRawFile);
		gRawFile = INVALID_HANDLE_VALUE;
		DeleteFileA(gRawPath.c_str());
	}
#else
	if(gRawFd >= 0){
		close(gRawFd);
		gRawFd = -1;
		unlink(gRawPath.c_str());
	}
#endif
}

void
CrashReportsStartup(void)
{
	if(!gInstalled)
		return;
	// Last runs' crashes, from processes that have ended.
	for(const std::string &name : listDir(gReportsDir, ".raw")){
		long pid = atol(name.c_str() + strlen("running-"));
		if(pid == currentPid() || (pid > 0 && processAlive(pid)))
			continue;
		convertRaw(joinDir(gReportsDir, name.c_str()));
	}
	refreshPending();
	// None kept past 30 days.
	time_t now = time(nil);
	for(const std::string &name : gPending){
		struct stat st;
		std::string path = joinDir(pendingDir(), name.c_str());
		if(stat(path.c_str(), &st) == 0 && now - st.st_mtime > KEEP_DAYS * 24 * 3600)
			remove(path.c_str());
	}
	refreshPending();
	loadSettings();
	if(gSendMode == SEND_NEVER && !gPending.empty()){
		log("Crash reports: %d report(s) deleted unsent (Never send)\n", (int)gPending.size());
		deletePending();
	}else if(gSendMode == SEND_ALWAYS)
		sendPending();

	// --crash-test[=abort|error], for the tests: only with HAUMEA_CRASH_TEST=1.
	const char *test = getenv("HAUMEA_CRASH_TEST");
	if(test && strcmp(test, "1") == 0)
		for(int i = 1; i < sk::args.argc; i++){
			const char *arg = sk::args.argv[i];
			if(strcmp(arg, "--crash-test=error") == 0){
				// One place twice: one report, counted. volatile: an unrolled loop
				// would be two places (two signatures) in a Release build.
				for(volatile int k = 0; k < 2; k++)
					CrashReportsError("crash test", "a deliberate error from %s", GetGameDir());
			}else if(strcmp(arg, "--crash-test=abort") == 0){
				log("Crash reports: aborting on purpose (--crash-test=abort)\n");
				abort();
			}else if(strcmp(arg, "--crash-test") == 0){
				log("Crash reports: crashing on purpose (--crash-test) in %s\n", GetGameDir());
				volatile int *nowhere = nil;
				*nowhere = 1;
			}
		}
}

void
CrashReportsLog(const char *text)
{
	// Several lines in one call become several lines here.
	while(gLogBusy.test_and_set(std::memory_order_acquire))
		;
	const char *p = text;
	while(*p){
		const char *eol = strchr(p, '\n');
		size_t len = eol ? (size_t)(eol - p) : strlen(p);
		if(len > 0){
			char *line = gLog[gLogNext.load() % LOG_LINES];
			size_t n = std::min(len, (size_t)LOG_LINE - 1);
			memcpy(line, p, n);
			line[n] = '\0';
			gLogNext++;
		}
		p += len + (eol ? 1 : 0);
	}
	gLogBusy.clear(std::memory_order_release);
}

void
CrashReportsSetContext(const char *mode, const char *window)
{
	snprintf(gMode, sizeof(gMode), "%s", mode ? mode : "");
	snprintf(gWindow, sizeof(gWindow), "%s", window ? window : "");
}

void
CrashReportsError(const char *where, const char *fmt, ...)
{
	if(!gInstalled || gReportsDir.empty())
		return;
	char message[1000];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(message, sizeof(message), fmt, ap);
	va_end(ap);
	std::vector<Frame> frames;
#ifdef _WIN32
	void *pcs[MAX_FRAMES];
	int n = CaptureStackBackTrace(1, MAX_FRAMES, pcs, nil);
#else
	void *pcs[MAX_FRAMES + 1];
	int n = backtrace(pcs, MAX_FRAMES + 1);
#endif
	for(int i = 0; i < n; i++){
#ifndef _WIN32
		if(i == 0)
			continue;	// this function
#endif
		const char *name;
		uintptr_t offset;
		char hex[24];
		if(moduleOf((uintptr_t)pcs[i], &name, &offset)){
			snprintf(hex, sizeof(hex), "0x%llx", (unsigned long long)offset);
			frames.push_back({ name, hex });
		}else{
			snprintf(hex, sizeof(hex), "0x%llx", (unsigned long long)(uintptr_t)pcs[i]);
			frames.push_back({ "?", hex });
		}
	}
	if(frames.empty())
		return;
	std::string sig = signatureOf("error", scrub(where), frames);
	static std::map<std::string, std::pair<std::string, int>> seen;	// signature: id, count
	auto it = seen.find(sig);
	if(it == seen.end() && (int)seen.size() >= MAX_ERRORS_PER_SESSION)
		return;
	ReportFields r;
	r.kind = "error";
	r.where = where;
	r.message = message;
	r.frames = frames;
	r.created = time(nil);
	r.mode = gMode;
	r.window = gWindow;
	unsigned next = gLogNext.load();
	for(unsigned i = next > LOG_LINES ? next - LOG_LINES : 0; i < next; i++)
		r.log.push_back(gLog[i % LOG_LINES]);
	std::string text(gHeader, gHeaderLen);
	for(size_t at = 0; at < text.size();){
		size_t eol = text.find('\n', at), tab = text.find('\t', at);
		if(eol == std::string::npos || tab == std::string::npos || tab > eol)
			break;
		r.header[text.substr(at, tab - at)] = text.substr(tab + 1, eol - tab - 1);
		at = eol + 1;
	}
	std::string id;
	if(it != seen.end()){
		// The same error again: its report counts it.
		r.count = ++it->second.second;
		if(r.count > 1000)
			return;
		std::string json = reportJson(r, &id);
		std::string keep = it->second.first;
		size_t at = json.find("\"id\":\"");
		if(at != std::string::npos)
			json.replace(at + 6, 32, keep);
		if(!gSending)
			savePending(json, keep);
		return;
	}
	std::string json = reportJson(r, &id);
	if(savePending(json, id)){
		seen[sig] = { id, 1 };
		refreshPending();
	}
}

// A button in the theme's accent with white text: a row's main action.
static bool
accentButton(const char *label)
{
	ImVec4 fill = UiAccentFill();
	ImGui::PushStyleColor(ImGuiCol_Button, fill);
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(fill.x*1.12f, fill.y*1.12f, fill.z*1.12f, 1.0f));
	ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(fill.x*0.9f, fill.y*0.9f, fill.z*0.9f, 1.0f));
	ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
	bool pressed = ImGui::Button(label);
	ImGui::PopStyleColor(4);
	return pressed;
}

static float
buttonWidth(const char *label)
{
	return ImGui::CalcTextSize(label, nil, true).x + ImGui::GetStyle().FramePadding.x * 2.0f;
}

// The report viewer: every waiting report, searchable, filling the window.
static void
reportViewer(void)
{
	std::string titleText = std::string(_("Report")) + "###crash_report_viewer";
	const char *title = titleText.c_str();
	ImGuiWindowFlags placed = UiPlaceToolWindow(title, 40.0f, false);
	if(ImGui::Begin(title, &gShowViewer, placed)){
		static char search[128];
		if(!TelemetryInstallId().empty())
			ImGui::TextDisabled("%s", _("Sent with your install ID too, since the daily ping is on."));
		ImGui::SetNextItemWidth(-FLT_MIN);
		ImGui::InputTextWithHint("##report_search", _("Search"), search, sizeof(search));
		ImGui::BeginChild("##report_list", ImVec2(0, 0), ImGuiChildFlags_Borders);
		bool any = false;
		for(const std::string &name : gPending){
			std::string body;
			readFile(joinDir(pendingDir(), name.c_str()), &body);
			if(search[0] == '\0'){
				any = true;
				if(UiSection(name.c_str(), ImGuiTreeNodeFlags_DefaultOpen))
					ImGui::TextWrapped("%s", body.c_str());
				continue;
			}
			// While searching: each report's lines that match, under its name.
			std::string lowered = body, needle = search;
			for(char &c : lowered) c = (char)tolower((unsigned char)c);
			for(char &c : needle) c = (char)tolower((unsigned char)c);
			if(lowered.find(needle) == std::string::npos)
				continue;
			any = true;
			if(!UiSection(name.c_str(), ImGuiTreeNodeFlags_DefaultOpen))
				continue;
			for(size_t at = 0; at < body.size();){
				size_t eol = body.find('\n', at);
				if(eol == std::string::npos)
					eol = body.size();
				if(lowered.substr(at, eol - at).find(needle) != std::string::npos)
					ImGui::TextWrapped("%.*s", (int)(eol - at), body.c_str() + at);
				at = eol + 1;
			}
		}
		if(!any)
			ImGui::TextDisabled("%s", _("Nothing matches."));
		ImGui::EndChild();
	}
	ImGui::End();
}

// After a crash or an error: send the waiting reports or not. What happened
// and what is left out, then View report at the left and the answers at the
// right, Send in the accent.
void
CrashReportsDrawBanner(void)
{
	if(!gInstalled || gBannerDismissed || gSendMode != SEND_ASK || gPending.empty() || blockedBecause())
		return;
	ImGuiViewport *view = ImGui::GetMainViewport();
	ImGui::SetNextWindowPos(ImVec2(view->WorkPos.x + view->WorkSize.x * 0.5f, view->WorkPos.y + 40.0f),
		ImGuiCond_Always, ImVec2(0.5f, 0.0f));
	ImGuiWindowFlags flags = ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse |
		ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoMove;
	if(ImGui::Begin((std::string(_("Send a report?")) + "###crash_reports").c_str(), nil, flags)){
		ImGui::TextUnformatted(gPending.size() == 1 ? _("Haumea Studio closed unexpectedly or hit an error. A report can help fix it.") :
			_("Haumea Studio closed unexpectedly or hit errors. Reports can help fix them."));
		ImGui::TextDisabled("%s", gSending ? _("Sending...") : _("Your folders, user name and map names are left out."));
		ImGui::Spacing();
		if(ImGui::Button(_("View report")))
			gShowViewer = true;
		// The answers at the row's right end: Don't send, Always send, Send.
		float spacing = ImGui::GetStyle().ItemSpacing.x;
		float answers = buttonWidth(_("Don't send")) + buttonWidth(_("Always send")) + buttonWidth(_("Send")) + spacing * 2.0f;
		float right = ImGui::GetWindowContentRegionMax().x;
		ImGui::SameLine();
		if(ImGui::GetCursorPosX() < right - answers)
			ImGui::SetCursorPosX(right - answers);
		if(ImGui::Button(_("Don't send"))){
			deletePending();
			gBannerDismissed = true;
		}
		ImGui::SameLine();
		if(ImGui::Button(_("Always send"))){
			gSendMode = SEND_ALWAYS;
			saveSettings();
			sendPending();
		}
		ImGui::SetItemTooltip("%s", _("Preferences > Privacy changes it later."));
		ImGui::SameLine();
		if(accentButton(_("Send")))
			sendPending();
	}
	ImGui::End();
	if(gShowViewer)
		reportViewer();
}

void
CrashReportsPreferences(void)
{
	loadSettings();
	UiHeading(_("Crash and error reports"));
	int mode = gSendMode;
	bool changed = ImGui::RadioButton(_("Ask"), &mode, SEND_ASK);
	ImGui::SameLine();
	changed |= ImGui::RadioButton(_("Always send"), &mode, SEND_ALWAYS);
	ImGui::SameLine();
	changed |= ImGui::RadioButton(_("Never send"), &mode, SEND_NEVER);
	if(changed){
		gSendMode = mode;
		saveSettings();
		if(mode == SEND_NEVER)
			deletePending();
		else if(mode == SEND_ALWAYS)
			sendPending();
	}
	ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
	ImGui::TextWrapped(_("After a crash or an error: the version, system and graphics card, where in Haumea's "
		"code it happened and the last lines of its log, with your folders, user name and map names left out. "
		"With the daily ping on, the install ID too. Reports not sent are deleted after %d days."), KEEP_DAYS);
	ImGui::PopStyleColor();
	if(!gPending.empty())
		ImGui::TextDisabled(_("Reports waiting: %d"), (int)gPending.size());
	ImGui::PushID("reports");
	if(ImGui::Button(_("What is sent and kept...")))
		OpenInFileManager(PRIVACY_URL);
	ImGui::PopID();
}
