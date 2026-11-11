// Translation: UTF-8 .po catalogs read at run time (no gettext), so a new
// language is a lang/<code>.po file and nothing else.
#include "haumea.h"
#include "i18n.h"
#include "imgui/imgui_internal.h"

#include <algorithm>
#include <map>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <unordered_map>
#include <vector>
#include <sys/stat.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>	// MSVC has none
#endif

static std::unordered_map<std::string, std::string> catalog;
// By the literal's address, which _() runs for every label every frame; the
// text is kept with it, so _(buffer) with a changing buffer is not stale.
struct Cached {
	std::string english;
	std::string label;	// Label/LabelIcon/TextIcon results
	const char *result = nil;	// Translate
};
static std::unordered_map<const char *, Cached> literalCache, labelCache;
typedef std::pair<const char *, const char *> LiteralPair;
static std::map<LiteralPair, Cached> labelIconCache, textIconCache;
static std::vector<I18nLanguage> languages;
static bool languagesScanned;
static std::string preference = "auto";
static std::string activeCode = "en";
static bool dirty = true;

static bool
isDir(const std::string &path)
{
	struct stat st;
	return stat(path.c_str(), &st) == 0 && (st.st_mode & S_IFDIR);
}

// lang beside the editor (a release), or tools/haumea/lang in the source tree
// above a developer build. HAUMEA_LANG_DIR moves it (tests).
static std::string
langDir(void)
{
	const char *custom = getenv("HAUMEA_LANG_DIR");
	if(custom && custom[0])
		return custom;
	char root[1024];
	if(!GetEditorRootDirectory(root, sizeof(root)))
		return "";
	std::string dir = root;
	for(int up = 0; up < 5; up++){
		if(isDir(dir + "/lang"))
			return dir + "/lang";
		if(isDir(dir + "/tools/haumea/lang"))
			return dir + "/tools/haumea/lang";
		size_t slash = dir.find_last_of("/\\");
		if(slash == std::string::npos || slash == 0)
			break;
		dir.resize(slash);
	}
	return "";
}

static bool
readFile(const std::string &path, std::string &out)
{
	FILE *f = fopen(path.c_str(), "rb");
	if(f == nil)
		return false;
	char buf[8192];
	size_t n;
	out.clear();
	while((n = fread(buf, 1, sizeof(buf), f)) > 0)
		out.append(buf, n);
	fclose(f);
	return true;
}

// The conversion characters of a printf format in order ("%d items %s" is
// "ds"), "%%" skipped. A translation must have the same ones or the call
// would read the wrong arguments.
static std::string
conversions(const std::string &fmt)
{
	std::string out;
	for(size_t i = 0; i < fmt.size(); i++){
		if(fmt[i] != '%')
			continue;
		i++;
		if(i < fmt.size() && fmt[i] == '%')
			continue;
		while(i < fmt.size() && strchr("-+ #0123456789.*", fmt[i]))
			i++;
		while(i < fmt.size() && strchr("hlLzjtq", fmt[i]))
			i++;
		if(i < fmt.size())
			out += fmt[i];
	}
	return out;
}

// One quoted .po string starting at s[i] == '"'; advances i past it.
static bool
poString(const std::string &s, size_t &i, std::string &out)
{
	if(i >= s.size() || s[i] != '"')
		return false;
	i++;
	while(i < s.size() && s[i] != '"'){
		if(s[i] == '\\' && i + 1 < s.size()){
			i++;
			switch(s[i]){
			case 'n': out += '\n'; break;
			case 't': out += '\t'; break;
			case 'r': out += '\r'; break;
			default: out += s[i]; break;
			}
		}else
			out += s[i];
		i++;
	}
	if(i >= s.size())
		return false;
	i++;
	return true;
}

struct PoEntry {
	std::string id, str;
	bool fuzzy = false;
};

// Entries in file order; the header is the entry with an empty id.
static std::vector<PoEntry>
parsePo(const std::string &text)
{
	std::vector<PoEntry> entries;
	PoEntry cur;
	std::string *target = nil;
	bool have = false, fuzzyPending = false;
	size_t pos = 0;
	auto flush = [&](){
		if(have)
			entries.push_back(cur);
		cur = PoEntry();
		cur.fuzzy = false;
		have = false;
		target = nil;
	};
	while(pos < text.size()){
		size_t end = text.find('\n', pos);
		if(end == std::string::npos)
			end = text.size();
		std::string line = text.substr(pos, end - pos);
		pos = end + 1;
		while(!line.empty() && (line.back() == '\r' || line.back() == ' '))
			line.pop_back();
		size_t i = 0;
		while(i < line.size() && (line[i] == ' ' || line[i] == '\t'))
			i++;
		if(i >= line.size()){
			flush();
			continue;
		}
		if(line[i] == '#'){
			if(line.compare(i, 2, "#,") == 0 && line.find("fuzzy") != std::string::npos)
				fuzzyPending = true;
			continue;
		}
		if(line.compare(i, 5, "msgid") == 0 && !isalpha((unsigned char)line[i + 5]) && line.compare(i, 12, "msgid_plural") != 0){
			flush();
			cur.fuzzy = fuzzyPending;
			fuzzyPending = false;
			have = true;
			target = &cur.id;
			i += 5;
		}else if(line.compare(i, 6, "msgstr") == 0 && line.compare(i, 7, "msgstr[") != 0){
			target = &cur.str;
			i += 6;
		}else if(line.compare(i, 7, "msgstr[") == 0){
			// Plural forms are not used; read only the first.
			target = line.compare(i, 8, "msgstr[0") == 0 ? &cur.str : nil;
			i = line.find(']', i);
			if(i == std::string::npos)
				continue;
			i++;
		}else if(line.compare(i, 7, "msgctxt") == 0){
			flush();
			continue;
		}else if(line.compare(i, 12, "msgid_plural") == 0){
			target = nil;
			continue;
		}
		while(i < line.size() && (line[i] == ' ' || line[i] == '\t'))
			i++;
		if(target && i < line.size() && line[i] == '"')
			poString(line, i, *target);
	}
	flush();
	return entries;
}

static std::string
headerField(const std::string &header, const char *name)
{
	std::string key = std::string(name) + ":";
	size_t at = header.find(key);
	if(at == std::string::npos)
		return "";
	at += key.size();
	while(at < header.size() && header[at] == ' ')
		at++;
	size_t end = header.find('\n', at);
	return header.substr(at, end == std::string::npos ? std::string::npos : end - at);
}

static void
clearCaches(void)
{
	literalCache.clear();
	labelCache.clear();
	labelIconCache.clear();
	textIconCache.clear();
}

bool
I18nLoadCatalogText(const std::string &po, int *dropped)
{
	if(dropped)
		*dropped = 0;
	std::unordered_map<std::string, std::string> fresh;
	for(const PoEntry &e : parsePo(po)){
		if(e.id.empty() || e.str.empty() || e.fuzzy)
			continue;
		if(conversions(e.id) != conversions(e.str)){
			if(dropped)
				(*dropped)++;
			continue;
		}
		fresh[e.id] = e.str;
	}
	catalog.swap(fresh);
	clearCaches();
	return true;
}

static void
scanLanguages(void)
{
	languagesScanned = true;
	languages.clear();
	std::string dir = langDir();
	if(dir.empty())
		return;
	std::vector<std::string> names;
#ifdef _WIN32
	WIN32_FIND_DATAA fd;
	HANDLE h = FindFirstFileA((dir + "/*.po").c_str(), &fd);
	if(h == INVALID_HANDLE_VALUE)
		return;
	do
		if(!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
			names.push_back(fd.cFileName);
	while(FindNextFileA(h, &fd));
	FindClose(h);
#else
	DIR *d = opendir(dir.c_str());
	if(d == nil)
		return;
	while(struct dirent *e = readdir(d))
		names.push_back(e->d_name);
	closedir(d);
#endif
	for(const std::string &name : names){
		if(name.size() < 4 || name.compare(name.size() - 3, 3, ".po") != 0)
			continue;
		I18nLanguage lang;
		lang.code = name.substr(0, name.size() - 3);
		lang.name = lang.code;
		std::string text;
		if(readFile(dir + "/" + name, text)){
			for(const PoEntry &entry : parsePo(text)){
				if(entry.id.empty()){
					std::string n = headerField(entry.str, "Language-Name");
					if(!n.empty())
						lang.name = n;
					break;
				}
			}
		}
		languages.push_back(lang);
	}
	std::sort(languages.begin(), languages.end(),
		[](const I18nLanguage &a, const I18nLanguage &b){ return a.code < b.code; });
}

const std::vector<I18nLanguage> &
I18nLanguages(void)
{
	if(!languagesScanned)
		scanLanguages();
	return languages;
}

// "pt_BR.UTF-8" -> "pt-br", lower case with a dash.
static std::string
normaliseLocale(std::string s)
{
	size_t cut = s.find_first_of(".@");
	if(cut != std::string::npos)
		s.resize(cut);
	for(char &c : s){
		if(c == '_')
			c = '-';
		c = (char)tolower((unsigned char)c);
	}
	return s;
}

static std::string
osLocale(void)
{
#ifdef _WIN32
	wchar_t w[LOCALE_NAME_MAX_LENGTH];
	if(GetUserDefaultLocaleName(w, LOCALE_NAME_MAX_LENGTH) > 0){
		char u[LOCALE_NAME_MAX_LENGTH * 4];
		if(WideCharToMultiByte(CP_UTF8, 0, w, -1, u, sizeof(u), nil, nil) > 0)
			return u;
	}
	return "";
#else
	const char *vars[] = { "LC_ALL", "LC_MESSAGES", "LANG" };
	for(const char *v : vars){
		const char *val = getenv(v);
		if(val && val[0] && strcmp(val, "C") != 0 && strcmp(val, "POSIX") != 0)
			return val;
	}
	return "";
#endif
}

// The catalog code for a locale: an exact match ("pt-BR"), else the first
// catalog with the same language ("pt" -> pt-BR); "" for none.
static std::string
catalogFor(const std::string &locale)
{
	std::string want = normaliseLocale(locale);
	if(want.empty())
		return "";
	std::string base = want.substr(0, want.find('-'));
	std::string baseMatch;
	for(const I18nLanguage &l : I18nLanguages()){
		std::string have = normaliseLocale(l.code);
		if(have == want)
			return l.code;
		if(baseMatch.empty() && have.substr(0, have.find('-')) == base)
			baseMatch = l.code;
	}
	return baseMatch;
}

static void
apply(void)
{
	dirty = false;
	std::string code;
	if(preference == "auto")
		code = catalogFor(osLocale());
	else if(preference != "en")
		code = catalogFor(preference);
	catalog.clear();
	clearCaches();
	activeCode = "en";
	if(code.empty())
		return;
	std::string dir = langDir(), text;
	if(dir.empty() || !readFile(dir + "/" + code + ".po", text))
		return;
	int dropped = 0;
	I18nLoadCatalogText(text, &dropped);
	activeCode = code;
	if(dropped)
		fprintf(stderr, "Language %s: %d translations ignored (printf conversions differ from English)\n",
			code.c_str(), dropped);
}

const char *
I18nGetLanguage(void)
{
	return preference.c_str();
}

void
I18nSetLanguage(const char *p)
{
	std::string next = p ? p : "";
	while(!next.empty() && isspace((unsigned char)next.back()))	// a settings line keeps its newline
		next.pop_back();
	if(next.empty())
		next = "auto";
	if(next != preference){
		preference = next;
		dirty = true;
	}
}

void
I18nInit(void)
{
	scanLanguages();
	apply();
}

const char *
I18nActiveLanguage(void)
{
	if(dirty)
		apply();
	return activeCode.c_str();
}

void
I18nNewFrame(void)
{
	if(dirty)
		apply();
}

const char *
Translate(const char *text)
{
	if(dirty)
		apply();
	if(catalog.empty())
		return text;
	Cached &c = literalCache[text];
	if(c.result && c.english == text)
		return c.result;
	c.english = text;
	c.result = text;
	const char *id = strstr(text, "##");
	if(id){
		auto it = catalog.find(std::string(text, id - text));
		if(it != catalog.end()){
			c.label = it->second + id;
			c.result = c.label.c_str();
		}
	}else if(!id){
		auto it = catalog.find(text);
		if(it != catalog.end())
			c.result = it->second.c_str();
	}
	return c.result;
}

// A window saved under its old title (the English text, with the icon
// first for LI) is moved to the ### ID's settings, so layouts and docks
// survive the switch to L()/LI() labels. A no-op once moved or for labels
// that were never windows.
static void
migrateWindowSettings(const char *oldTitle, const char *english)
{
	if(ImGui::GetCurrentContext() == nil)
		return;
	std::string idTitle = std::string("###") + english;
	ImGuiID newId = ImHashStr(idTitle.c_str());
	ImGuiID oldId = ImHashStr(oldTitle);
	if(oldId == newId || ImGui::FindWindowSettingsByID(newId))
		return;
	ImGuiWindowSettings *old = ImGui::FindWindowSettingsByID(oldId);
	if(old == nil)
		return;
	ImGuiWindowSettings copy = *old;
	old->WantDelete = true;
	ImGuiWindowSettings *moved = ImGui::CreateNewWindowSettings(idTitle.c_str());
	ImGuiID keepId = moved->ID;
	*moved = copy;
	moved->ID = keepId;
	moved->WantDelete = false;

	// The dock node that shows it as the selected tab remembers the old ID.
	ImGuiContext &g = *ImGui::GetCurrentContext();
	for(int n = 0; n < g.DockContext.Nodes.Data.Size; n++){
		ImGuiDockNode *node = (ImGuiDockNode*)g.DockContext.Nodes.Data[n].val_p;
		if(node == nil)
			continue;
		if(node->SelectedTabId == oldId)
			node->SelectedTabId = newId;
		if(node->TabBar && node->TabBar->SelectedTabId == oldId)
			node->TabBar->SelectedTabId = newId;
		if(node->TabBar && node->TabBar->NextSelectedTabId == oldId)
			node->TabBar->NextSelectedTabId = newId;
	}
}

const char *
Label(const char *english)
{
	if(dirty)
		apply();
	Cached &c = labelCache[english];
	std::string &s = c.label;
	if(s.empty() || c.english != english){
		c.english = english;
		migrateWindowSettings(english, english);
		s = Translate(english);
		s += "###";
		s += english;
	}
	return s.c_str();
}

const char *
LabelIcon(const char *icon, const char *english)
{
	if(dirty)
		apply();
	Cached &c = labelIconCache[LiteralPair(icon, english)];
	std::string &s = c.label;
	if(s.empty() || c.english != english){
		c.english = english;
		std::string oldTitle = std::string(icon) + " " + english;
		migrateWindowSettings(oldTitle.c_str(), english);
		s = icon;
		s += " ";
		s += Translate(english);
		s += "###";
		s += english;
	}
	return s.c_str();
}

const char *
TextIcon(const char *icon, const char *english)
{
	if(dirty)
		apply();
	Cached &c = textIconCache[LiteralPair(icon, english)];
	std::string &s = c.label;
	if(s.empty() || c.english != english){
		c.english = english;
		s = icon;
		s += " ";
		s += Translate(english);
	}
	return s.c_str();
}

bool
uiLanguagePreferences(void)
{
	bool changed = false;
	const std::vector<I18nLanguage> &langs = I18nLanguages();
	std::string current = I18nGetLanguage();
	std::string shown;
	if(current == "auto")
		shown = std::string(_("Auto")) + " (" + I18nActiveLanguage() + ")";
	else if(current == "en")
		shown = "English";
	else{
		shown = current;
		for(const I18nLanguage &l : langs)
			if(l.code == current)
				shown = l.name;
	}
	// Its name beside it, like every other field of the page.
	std::string label = std::string(_("Language")) + "###language";
	if(ImGui::BeginCombo(label.c_str(), shown.c_str())){
		if(ImGui::Selectable(_("Auto"), current == "auto"))
			{ I18nSetLanguage("auto"); changed = true; }
		if(ImGui::Selectable("English", current == "en"))
			{ I18nSetLanguage("en"); changed = true; }
		for(const I18nLanguage &l : langs)
			if(ImGui::Selectable(l.name.c_str(), current == l.code))
				{ I18nSetLanguage(l.code.c_str()); changed = true; }
		ImGui::EndCombo();
	}
	ImGui::SetItemTooltip("%s", _("Auto follows the system language when Haumea has it, else English.\nA language is a lang/<code>.po file beside the editor."));
	return changed;
}

// The non-ASCII characters of a UTF-8 string, appended to out. A stray or
// truncated sequence is skipped, not guessed at.
static void
addCodepoints(const std::string &text, std::vector<unsigned int> &out)
{
	for(size_t i = 0; i < text.size(); ){
		unsigned char c = (unsigned char)text[i];
		unsigned int cp;
		if(c < 0x80){
			i += 1;
			continue;
		}else if((c & 0xE0) == 0xC0 && i + 1 < text.size()){
			cp = ((c & 0x1Fu) << 6) | ((unsigned char)text[i + 1] & 0x3Fu);
			i += 2;
		}else if((c & 0xF0) == 0xE0 && i + 2 < text.size()){
			cp = ((c & 0x0Fu) << 12) | (((unsigned char)text[i + 1] & 0x3Fu) << 6)
				| ((unsigned char)text[i + 2] & 0x3Fu);
			i += 3;
		}else if((c & 0xF8) == 0xF0 && i + 3 < text.size()){
			cp = ((c & 0x07u) << 18) | (((unsigned char)text[i + 1] & 0x3Fu) << 12)
				| (((unsigned char)text[i + 2] & 0x3Fu) << 6) | ((unsigned char)text[i + 3] & 0x3Fu);
			i += 4;
		}else{
			i += 1;
			continue;
		}
		// The atlas is 16-bit: astral characters are out of reach, and the
		// surrogate block is not a character.
		if(cp >= 0x80 && cp <= 0xFFFF && (cp < 0xD800 || cp > 0xDFFF))
			out.push_back(cp);
	}
}

void
I18nCatalogCodepoints(std::vector<unsigned int> &out)
{
	std::string dir = langDir();
	if(dir.empty())
		return;
	for(const I18nLanguage &lang : I18nLanguages()){
		std::string text;
		if(!readFile(dir + "/" + lang.code + ".po", text))
			continue;
		for(const PoEntry &entry : parsePo(text))
			if(!entry.id.empty())
				addCodepoints(entry.str, out);
	}
}
