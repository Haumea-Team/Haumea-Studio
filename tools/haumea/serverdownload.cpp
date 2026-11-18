// SPDX-License-Identifier: GPL-2.0-or-later
#include "haumea.h"
#include "serverdownload.h"
#include "simplejson.h"
#include "updatecore.h"
#include "updater.h"

#include <atomic>
#include <mutex>
#include <stdlib.h>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#endif

// The latest open.mp server release, downloaded into a folder in the
// background: the release's archive for this platform is unpacked next to
// it, and its Server folder merged in without replacing anything already
// there (the workspace's own gamemodes, config.json and so on). The streamer
// plugin's release goes in the same way: its plugins folder, and its include
// into qawno/include, where open.mp's pawncc looks.

static const char *const RELEASE_URL = "https://api.github.com/repos/openmultiplayer/open.mp/releases/latest";
#ifdef _WIN32
static const char *const ASSET_NAME = "open.mp-win-x86.zip";
#else
static const char *const ASSET_NAME = "open.mp-linux-x86.tar.gz";
#endif
static const char *const STREAMER_URL = "https://api.github.com/repos/samp-incognito/samp-streamer-plugin/releases/latest";

// When the API won't answer (GitHub allows 60 calls an hour per address,
// which a shared connection runs out of), the release pages' own links, which
// aren't limited: open.mp's latest by its fixed name, and the streamer's last
// release (its archive is named after the version).
static const char *const OPENMP_DIRECT = "https://github.com/openmultiplayer/open.mp/releases/latest/download/";
static const char *const STREAMER_DIRECT_TAG = "v2.9.6";
static const char *const STREAMER_DIRECT =
	"https://github.com/samp-incognito/samp-streamer-plugin/releases/download/v2.9.6/samp-streamer-plugin-2.9.6.zip";

static std::atomic<int> gState(SERVER_DOWNLOAD_IDLE);
static std::atomic<int> gOwner(SERVER_DOWNLOAD_FOR_WORKSPACE);
static std::mutex gLock;
static std::string gDir, gMessage, gPart;

static void
finish(int state, const std::string &message)
{
	std::lock_guard<std::mutex> lock(gLock);
	gMessage = message;
	gState = state;
}

static std::vector<std::string>
listDir(const std::string &dir)
{
	std::vector<std::string> names;
#ifdef _WIN32
	WIN32_FIND_DATAA data;
	HANDLE h = FindFirstFileA((dir + "\\*").c_str(), &data);
	if(h == INVALID_HANDLE_VALUE)
		return names;
	do{
		if(strcmp(data.cFileName, ".") != 0 && strcmp(data.cFileName, "..") != 0)
			names.push_back(data.cFileName);
	}while(FindNextFileA(h, &data));
	FindClose(h);
#else
	DIR *d = opendir(dir.c_str());
	if(d == nil)
		return names;
	while(struct dirent *e = readdir(d))
		if(strcmp(e->d_name, ".") != 0 && strcmp(e->d_name, "..") != 0)
			names.push_back(e->d_name);
	closedir(d);
#endif
	return names;
}

// Moves what from has into to, folder by folder; a file to already has stays.
static void
merge(const std::string &from, const std::string &to)
{
	for(const std::string &name : listDir(from)){
		std::string src = from + "/" + name, dst = to + "/" + name;
		if(!UpdateFileExists(dst) && !UpdateIsDirectory(dst))
			rename(src.c_str(), dst.c_str());
		else if(UpdateIsDirectory(src) && UpdateIsDirectory(dst))
			merge(src, dst);
	}
}

// The folder in the archive with the server in it (open.mp's is "Server").
static std::string
findServer(const std::string &dir, int depth)
{
	for(const char *exe : { "omp-server", "omp-server.exe" })
		if(UpdateFileExists(dir + "/" + exe))
			return dir;
	if(depth > 0)
		for(const std::string &name : listDir(dir))
			if(UpdateIsDirectory(dir + "/" + name)){
				std::string found = findServer(dir + "/" + name, depth - 1);
				if(!found.empty())
					return found;
			}
	return "";
}

// A release's asset whose name passes match: its URL and the release tag.
template <typename F> static bool
findAsset(const std::string &releaseUrl, const std::string &work, F match,
          std::string *name, std::string *url, std::string *tag, std::string *error)
{
	std::string json, list = work + "/release.json";
	if(!HttpDownload(releaseUrl, list, error) || !UpdateReadFile(list, &json)){
		if(error->empty())
			*error = "no release list";
		return false;
	}
	HaumeaJson::Json root;
	HaumeaJson::JsonParser parser = { json.data(), json.data() + json.size() };
	if(parser.value(root) && root.type == HaumeaJson::Json::OBJ){
		*tag = root.text("tag_name");
		if(const HaumeaJson::Json *assets = root.get("assets"))
			for(const HaumeaJson::Json &a : assets->items)
				if(match(a.text("name"))){
					*name = a.text("name");
					*url = a.text("browser_download_url");
					return true;
				}
	}
	*error = "the latest release at " + releaseUrl + " has no archive for this platform";
	return false;
}

// Downloads url into work and unpacks it into work/unpacked.
static bool
fetchAndUnpack(const std::string &url, const std::string &name, const std::string &work,
               std::string *unpacked, std::string *error)
{
	std::string archive = work + "/" + name;
	{
		std::lock_guard<std::mutex> lock(gLock);
		gPart = archive;
	}
	if(!HttpDownload(url, archive, error))
		return false;
	*unpacked = work + "/unpacked";
	UpdateRemoveTree(*unpacked);
	UpdateMakeDirs(*unpacked);
	bool zip = name.size() > 4 && name.compare(name.size() - 4, 4, ".zip") == 0;
#ifdef _WIN32
	// Windows 10's tar (bsdtar) reads zip files too.
	(void)zip;
	std::string command = "tar -xf \"" + archive + "\" -C \"" + *unpacked + "\"";
#else
	std::string command = zip ? "unzip -q -o '" + archive + "' -d '" + *unpacked + "'"
		: "tar -xzf '" + archive + "' -C '" + *unpacked + "'";
#endif
	if(system(command.c_str()) != 0){
		*error = "could not unpack " + name + (zip ? " (is unzip installed?)" : "");
		return false;
	}
	return true;
}

static bool
fetchOpenMp(const std::string &dir, const std::string &work, std::string *tag, std::string *error)
{
	std::string name, url, unpacked;
	if(!findAsset(RELEASE_URL, work, [](const std::string &n){ return n == ASSET_NAME; },
	              &name, &url, tag, error)){
		name = ASSET_NAME;
		url = std::string(OPENMP_DIRECT) + ASSET_NAME;
		*tag = "latest";
		error->clear();
	}
	if(!fetchAndUnpack(url, name, work, &unpacked, error))
		return false;
	std::string server = findServer(unpacked, 2);
	if(server.empty()){
		*error = "no omp-server in " + name;
		return false;
	}
	merge(server, dir);
	return true;
}

static bool
fetchStreamer(const std::string &dir, const std::string &work, std::string *tag, std::string *error)
{
	// samp-streamer-plugin-<version>.zip has both platforms; not the debug one.
	std::string name, url, unpacked;
	if(!findAsset(STREAMER_URL, work, [](const std::string &n){
	                  return n.compare(0, 21, "samp-streamer-plugin-") == 0 &&
	                         n.find("debug") == std::string::npos &&
	                         n.size() > 4 && n.compare(n.size() - 4, 4, ".zip") == 0; },
	              &name, &url, tag, error)){
		url = STREAMER_DIRECT;
		name = url.substr(url.rfind('/') + 1);
		*tag = STREAMER_DIRECT_TAG;
		error->clear();
	}
	if(!fetchAndUnpack(url, name, work, &unpacked, error))
		return false;
	// Its pawno/include is open.mp's qawno/include.
	if(UpdateIsDirectory(unpacked + "/pawno")){
		UpdateMakeDirs(unpacked + "/qawno");
		merge(unpacked + "/pawno", unpacked + "/qawno");
	}
	if(!UpdateFileExists(unpacked + "/qawno/include/streamer.inc")){
		*error = "no streamer.inc in " + name;
		return false;
	}
	UpdateMakeDirs(dir + "/plugins");
	UpdateMakeDirs(dir + "/qawno/include");
	merge(unpacked + "/plugins", dir + "/plugins");
	merge(unpacked + "/qawno", dir + "/qawno");
	return true;
}

static void
worker(std::string dir, int what)
{
	std::string work = dir + "/.haumea-download";
	UpdateRemoveTree(work);
	if(!UpdateMakeDirs(work)){
		finish(SERVER_DOWNLOAD_FAILED, "can't write in " + dir);
		return;
	}
	std::string error, tag, streamerTag;
	bool ok = (!(what & SERVER_DOWNLOAD_OPENMP) || fetchOpenMp(dir, work, &tag, &error)) &&
		(!(what & SERVER_DOWNLOAD_STREAMER) || fetchStreamer(dir, work, &streamerTag, &error));
	UpdateRemoveTree(work);
	if(!ok){
		finish(SERVER_DOWNLOAD_FAILED, error.empty() ? "the download failed" : error);
		return;
	}
	// The tags, for the toast: "v1.6.7.3218, streamer v2.9.6".
	std::string message = tag;
	if(!streamerTag.empty())
		message += (message.empty() ? "streamer " : ", streamer ") + streamerTag;
	finish(SERVER_DOWNLOAD_DONE, message);
}

bool
ServerDownloadHasServer(const char *dir)
{
	for(const char *exe : { "omp-server", "omp-server.exe", "samp03svr", "samp-server.exe" })
		if(UpdateFileExists(std::string(dir) + "/" + exe))
			return true;
	return false;
}

void
ServerDownloadStart(const char *dir, int what, int owner)
{
	if(gState == SERVER_DOWNLOAD_RUNNING)
		return;
	{
		std::lock_guard<std::mutex> lock(gLock);
		gDir = dir;
		gMessage.clear();
		gPart.clear();
	}
	gOwner = owner;
	gState = SERVER_DOWNLOAD_RUNNING;
	std::thread(worker, std::string(dir), what).detach();
}

int
ServerDownloadOwner(void)
{
	return gOwner;
}

int
ServerDownloadStatus(std::string *dir, std::string *message, float *megabytes)
{
	int state = gState;
	std::lock_guard<std::mutex> lock(gLock);
	if(dir) *dir = gDir;
	if(message) *message = gMessage;
	if(megabytes){
		struct stat st;
		*megabytes = !gPart.empty() && stat(gPart.c_str(), &st) == 0 ? st.st_size / (1024.0f * 1024.0f) : 0.0f;
	}
	return state;
}

void
ServerDownloadAcknowledge(void)
{
	int state = gState;
	if(state == SERVER_DOWNLOAD_DONE || state == SERVER_DOWNLOAD_FAILED)
		gState = SERVER_DOWNLOAD_IDLE;
}
