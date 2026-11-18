// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "simplejson.h"
#include <fstream>
#include <sstream>
#include <algorithm>

// A server's configured artwork directory, relative to its root unless absolute.
// open.mp's config.json takes precedence over the legacy server.cfg.
inline std::string
ServerArtworkDirectory(const std::string &server)
{
	std::string path = "models";
	std::ifstream jsonFile(server + "/config.json");
	if(jsonFile){
		std::ostringstream contents;
		contents << jsonFile.rdbuf();
		std::string text = contents.str();
		HaumeaJson::Json root;
		HaumeaJson::JsonParser parser = { text.data(), text.data() + text.size() };
		if(parser.value(root) && root.type == HaumeaJson::Json::OBJ){
			const HaumeaJson::Json *artwork = root.get("artwork");
			std::string configured = artwork ? artwork->text("models_path") : "";
			if(configured.empty()) configured = root.text("artwork.models_path");
			if(!configured.empty()) path = configured;
		}
	}else{
		std::ifstream cfg(server + "/server.cfg");
		std::string line;
		while(std::getline(cfg, line)){
			std::istringstream fields(line);
			std::string key;
			fields >> key;
			if(key != "artpath") continue;
			std::string value;
			std::getline(fields >> std::ws, value);
			size_t end = value.find_last_not_of(" \t\r");
			if(end == std::string::npos) continue;
			value.resize(end + 1);
			if(value.size() >= 2 && value.front() == '"' && value.back() == '"')
				value = value.substr(1, value.size()-2);
			if(!value.empty()) path = value;
		}
	}
	size_t begin = path.find_first_not_of(" \t\r\n");
	size_t end = path.find_last_not_of(" \t\r\n");
	path = begin == std::string::npos ? "models" : path.substr(begin, end-begin+1);
	std::replace(path.begin(), path.end(), '\\', '/');
	bool absolute = !path.empty() && (path[0] == '/' ||
		(path.size() > 2 && path[1] == ':' && path[2] == '/'));
	return absolute ? path : server + "/" + path;
}

// Whether the server sends custom models to players: 1 on, 0 off, -1 when
// it has no config.json or server.cfg to say. open.mp: "artwork": { "enable" }
// in config.json; SA-MP 0.3.DL: useartwork 1 in server.cfg.
inline int
ServerArtworkEnabled(const std::string &server)
{
	std::ifstream jsonFile(server + "/config.json");
	if(jsonFile){
		std::ostringstream contents;
		contents << jsonFile.rdbuf();
		std::string text = contents.str();
		HaumeaJson::Json root;
		HaumeaJson::JsonParser parser = { text.data(), text.data() + text.size() };
		if(!parser.value(root) || root.type != HaumeaJson::Json::OBJ)
			return -1;
		const HaumeaJson::Json *artwork = root.get("artwork");
		return artwork && artwork->flag("enable") ? 1 : 0;
	}
	std::ifstream cfg(server + "/server.cfg");
	if(!cfg)
		return -1;
	std::string line;
	while(std::getline(cfg, line)){
		std::istringstream fields(line);
		std::string key, value;
		fields >> key >> value;
		if(key == "useartwork")
			return value == "1" ? 1 : 0;
	}
	return 0;
}

// Turns artwork on in the server's config (config.json, else server.cfg),
// changing only that setting. False with error when it can't.
inline bool
ServerEnableArtwork(const std::string &server, std::string *error)
{
	std::string jsonPath = server + "/config.json", cfgPath = server + "/server.cfg";
	auto readAll = [](const std::string &path, std::string *out){
		std::ifstream in(path, std::ios::binary);
		if(!in) return false;
		std::ostringstream contents;
		contents << in.rdbuf();
		*out = contents.str();
		return true;
	};
	auto writeAll = [](const std::string &path, const std::string &text){
		std::ofstream out(path, std::ios::binary | std::ios::trunc);
		out << text;
		return (bool)out;
	};
	std::string text;
	if(readAll(jsonPath, &text)){
		// "artwork": { ... "enable": false ... }, or no "enable", or no "artwork".
		size_t key = text.find("\"artwork\"");
		size_t open = key == std::string::npos ? std::string::npos : text.find('{', key);
		if(open != std::string::npos){
			size_t close = text.find('}', open);
			size_t enable = text.find("\"enable\"", open);
			if(enable != std::string::npos && enable < close){
				size_t value = text.find_first_not_of(" \t\r\n:", enable + 8);
				if(value != std::string::npos && text.compare(value, 5, "false") == 0)
					text.replace(value, 5, "true");
			}else
				text.insert(open + 1, " \"enable\": true,");
		}else{
			size_t top = text.find('{');
			if(top == std::string::npos){
				*error = "config.json isn't a JSON object";
				return false;
			}
			text.insert(top + 1, "\n\t\"artwork\": { \"enable\": true },");
		}
		if(!writeAll(jsonPath, text)){
			*error = "can't write " + jsonPath;
			return false;
		}
		return true;
	}
	if(readAll(cfgPath, &text)){
		std::istringstream lines(text);
		std::string line, out;
		bool found = false;
		while(std::getline(lines, line)){
			std::istringstream fields(line);
			std::string key;
			fields >> key;
			if(key == "useartwork"){
				line = "useartwork 1";
				found = true;
			}
			out += line + "\n";
		}
		if(!found)
			out += "useartwork 1\n";
		if(!writeAll(cfgPath, out)){
			*error = "can't write " + cfgPath;
			return false;
		}
		return true;
	}
	*error = "the server has no config.json or server.cfg";
	return false;
}

