// SPDX-License-Identifier: GPL-2.0-or-later
#include "mpaccounts.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#define strcasecmp _stricmp
#endif

static const char *ROLE_NAMES[] = { "", "viewer", "builder", "admin", "owner" };

const char *
MpRoleName(int role)
{
	return role >= MP_ROLE_NONE && role <= MP_ROLE_OWNER ? ROLE_NAMES[role] : "";
}

int
MpParseRole(const std::string &name)
{
	for(int role = MP_ROLE_VIEWER; role <= MP_ROLE_OWNER; role++)
		if(name == ROLE_NAMES[role])
			return role;
	return MP_ROLE_NONE;
}

static std::string
toHex(const uint8_t *bytes, size_t size)
{
	static const char digits[] = "0123456789abcdef";
	std::string out;
	for(size_t i = 0; i < size; i++){
		out += digits[bytes[i] >> 4];
		out += digits[bytes[i] & 15];
	}
	return out;
}

static bool
fromHex(const std::string &text, uint8_t *bytes, size_t size)
{
	if(text.size() != size * 2)
		return false;
	for(size_t i = 0; i < size; i++){
		unsigned value = 0;
		if(sscanf(text.c_str() + i * 2, "%2x", &value) != 1)
			return false;
		bytes[i] = (uint8_t)value;
	}
	return true;
}

std::string
MpFingerprint(const uint8_t publicKey[32])
{
	uint8_t hash[16];
	crypto_blake2b(hash, sizeof(hash), publicKey, 32);
	std::string hex = toHex(hash, sizeof(hash)), out;
	for(size_t i = 0; i < hex.size(); i += 4)
		out += (i ? ":" : "") + hex.substr(i, 4);
	return out;
}

static bool
readWhole(const std::string &path, std::string *out)
{
	FILE *f = fopen(path.c_str(), "rb");
	if(f == nullptr)
		return false;
	out->clear();
	char buffer[16384];
	size_t n;
	while((n = fread(buffer, 1, sizeof(buffer), f)) > 0)
		out->append(buffer, n);
	fclose(f);
	return true;
}

// Private to the server's user: these files hold what lets one in.
static bool
writePrivate(const std::string &path, const std::string &bytes)
{
	std::string temporary = path + ".tmp";
	FILE *f = fopen(temporary.c_str(), "wb");
	bool ok = f && fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
	if(f && fclose(f) != 0)
		ok = false;
#ifndef _WIN32
	chmod(temporary.c_str(), 0600);
#else
	if(ok)
		remove(path.c_str());
#endif
	if(!ok || rename(temporary.c_str(), path.c_str()) != 0){
		remove(temporary.c_str());
		return false;
	}
	return true;
}

static void
makeFolder(const std::string &path)
{
#ifdef _WIN32
	_mkdir(path.c_str());
#else
	mkdir(path.c_str(), 0700);
#endif
}

static long long
modified(const std::string &path)
{
	struct stat st;
	return stat(path.c_str(), &st) == 0 ? (long long)st.st_mtime * 1000 + (long long)(st.st_size % 1000) : 0;
}

static std::vector<std::string>
fields(const std::string &line)
{
	std::vector<std::string> out;
	size_t from = 0;
	while(true){
		size_t tab = line.find('\t', from);
		out.push_back(line.substr(from, tab == std::string::npos ? std::string::npos : tab - from));
		if(tab == std::string::npos)
			return out;
		from = tab + 1;
	}
}

static std::vector<std::string>
lines(const std::string &text)
{
	std::vector<std::string> out;
	size_t at = 0;
	while(at < text.size()){
		size_t end = text.find('\n', at);
		std::string line = text.substr(at, end == std::string::npos ? std::string::npos : end - at);
		if(!line.empty() && line.back() == '\r')
			line.pop_back();
		if(!line.empty() && line[0] != '#')
			out.push_back(line);
		at = end == std::string::npos ? text.size() : end + 1;
	}
	return out;
}

bool
MpAccounts::open(const std::string &folder, std::string *error)
{
	mFolder = folder;
	makeFolder(folder);
	makeFolder(roomsFolder());
	// The secrets are made once and kept.
	std::string bytes;
	if(readWhole(folder + "/secret.key", &bytes) && bytes.size() == sizeof(mSecret))
		memcpy(mSecret, bytes.data(), sizeof(mSecret));
	else{
		MpRandom(mSecret, sizeof(mSecret));
		if(!writePrivate(folder + "/secret.key", std::string((const char*)mSecret, sizeof(mSecret)))){
			*error = "could not write " + folder + "/secret.key";
			mFolder.clear();
			return false;
		}
	}
	if(readWhole(folder + "/identity.key", &bytes) && bytes.size() == sizeof(mIdentitySecret))
		memcpy(mIdentitySecret, bytes.data(), sizeof(mIdentitySecret));
	else{
		uint8_t seed[32];
		MpRandom(seed, sizeof(seed));
		crypto_eddsa_key_pair(mIdentitySecret, mIdentityPublic, seed);
		if(!writePrivate(folder + "/identity.key", std::string((const char*)mIdentitySecret, sizeof(mIdentitySecret)))){
			*error = "could not write " + folder + "/identity.key";
			mFolder.clear();
			return false;
		}
	}
	memcpy(mIdentityPublic, mIdentitySecret + 32, sizeof(mIdentityPublic));	// Monocypher keeps it there
	return load(error);
}

long long
MpAccounts::stamps(void) const
{
	return modified(mFolder + "/accounts.txt") * 7 + modified(mFolder + "/invites.txt");
}

bool
MpAccounts::load(std::string *error)
{
	mAccounts.clear();
	mInvites.clear();
	std::string text;
	if(readWhole(mFolder + "/accounts.txt", &text))
		for(const std::string &line : lines(text)){
			std::vector<std::string> f = fields(line);
			MpAccount a;
			if(f.size() < 4 || !validName(f[0]) || (a.role = MpParseRole(f[1])) == MP_ROLE_NONE ||
			   !fromHex(f[2], a.salt, sizeof(a.salt)) || !fromHex(f[3], a.key, sizeof(a.key))){
				if(error)
					*error = mFolder + "/accounts.txt has a line it can't read: " + line.substr(0, 40);
				continue;
			}
			a.name = f[0];
			mAccounts.push_back(a);
		}
	if(readWhole(mFolder + "/invites.txt", &text))
		for(const std::string &line : lines(text)){
			std::vector<std::string> f = fields(line);
			MpInvite v;
			if(f.size() < 6 || (v.role = MpParseRole(f[1])) == MP_ROLE_NONE || !fromHex(f[2], v.salt, sizeof(v.salt)) ||
			   !fromHex(f[3], v.key, sizeof(v.key)))
				continue;
			v.id = f[0];
			v.uses = atoi(f[4].c_str());
			v.expires = atoll(f[5].c_str());
			v.by = f.size() > 6 ? f[6] : "";
			mInvites.push_back(v);
		}
	mLoaded = stamps();
	return true;
}

void
MpAccounts::reloadIfChanged(void)
{
	if(isOpen() && stamps() != mLoaded)
		load(nullptr);
}

bool
MpAccounts::saveAccounts(void)
{
	std::string out = "# haumea-server accounts: name, role, password salt and key. Change with haumea-server's commands.\n";
	for(const MpAccount &a : mAccounts)
		out += a.name + "\t" + MpRoleName(a.role) + "\t" + toHex(a.salt, sizeof(a.salt)) + "\t" + toHex(a.key, sizeof(a.key)) + "\n";
	bool ok = writePrivate(mFolder + "/accounts.txt", out);
	mLoaded = stamps();
	return ok;
}

bool
MpAccounts::saveInvites(void)
{
	std::string out = "# haumea-server invites: id, role, salt and key of the secret, uses left, expiry, who made it.\n";
	long long now = (long long)time(nullptr);
	for(const MpInvite &v : mInvites)
		if(v.uses > 0 && (v.expires == 0 || v.expires > now))
			out += v.id + "\t" + MpRoleName(v.role) + "\t" + toHex(v.salt, sizeof(v.salt)) + "\t" + toHex(v.key, sizeof(v.key)) +
				"\t" + std::to_string(v.uses) + "\t" + std::to_string(v.expires) + "\t" + v.by + "\n";
	bool ok = writePrivate(mFolder + "/invites.txt", out);
	mLoaded = stamps();
	return ok;
}

const MpAccount *
MpAccounts::find(const std::string &name) const
{
	for(const MpAccount &a : mAccounts)
		if(strcasecmp(a.name.c_str(), name.c_str()) == 0)
			return &a;
	return nullptr;
}

bool
MpAccounts::nameTaken(const std::string &name) const
{
	return find(name) != nullptr;
}

bool
MpAccounts::validName(const std::string &name)
{
	if(name.size() < 2 || name.size() >= MP_NAME_LEN)
		return false;
	for(unsigned char ch : name)
		if(!isalnum(ch) && ch != '_' && ch != '-' && ch != '.')
			return false;
	return true;
}

bool
MpAccounts::create(const std::string &name, const std::string &password, int role, std::string *error)
{
	if(!validName(name)){
		*error = "a name is 2-31 letters, digits, _ - or .";
		return false;
	}
	if(nameTaken(name)){
		*error = "that name is taken";
		return false;
	}
	if(password.size() < 8 || password.size() >= MP_PASSWORD_LEN){
		*error = "a password is 8 to 63 characters";
		return false;
	}
	MpAccount a;
	a.name = name;
	a.role = role;
	MpRandom(a.salt, sizeof(a.salt));
	MpPasswordKey(password, a.salt, a.key);
	mAccounts.push_back(a);
	if(!saveAccounts()){
		*error = "could not write " + mFolder + "/accounts.txt";
		mAccounts.pop_back();
		return false;
	}
	return true;
}

bool
MpAccounts::setRole(const std::string &name, int role)
{
	for(MpAccount &a : mAccounts)
		if(strcasecmp(a.name.c_str(), name.c_str()) == 0){
			a.role = role;
			return saveAccounts();
		}
	return false;
}

bool
MpAccounts::setPassword(const std::string &name, const std::string &password)
{
	for(MpAccount &a : mAccounts)
		if(strcasecmp(a.name.c_str(), name.c_str()) == 0){
			MpRandom(a.salt, sizeof(a.salt));
			MpPasswordKey(password, a.salt, a.key);
			return saveAccounts();
		}
	return false;
}

bool
MpAccounts::remove(const std::string &name)
{
	for(size_t i = 0; i < mAccounts.size(); i++)
		if(strcasecmp(mAccounts[i].name.c_str(), name.c_str()) == 0){
			mAccounts.erase(mAccounts.begin() + i);
			return saveAccounts();
		}
	return false;
}

std::vector<MpAccount>
MpAccounts::all(void) const
{
	return mAccounts;
}

std::string
MpAccounts::invite(int role, int uses, int days, const std::string &by)
{
	// Easy to read out: no 0/O or 1/l.
	static const char letters[] = "ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnpqrstuvwxyz23456789";
	uint8_t random[24];
	MpRandom(random, sizeof(random));
	MpInvite v;
	for(int i = 0; i < 6; i++)
		v.id += letters[random[i] % (sizeof(letters) - 1)];
	std::string secret;
	for(int i = 6; i < 24; i++)
		secret += letters[random[i] % (sizeof(letters) - 1)];
	v.role = role;
	v.uses = uses > 0 ? uses : 1;
	v.expires = days > 0 ? (long long)time(nullptr) + (long long)days * 86400 : 0;
	v.by = by;
	MpRandom(v.salt, sizeof(v.salt));
	MpPasswordKey(secret, v.salt, v.key);
	mInvites.push_back(v);
	saveInvites();
	crypto_wipe(random, sizeof(random));
	return v.id + "-" + secret;
}

const MpInvite *
MpAccounts::findInvite(const std::string &id) const
{
	long long now = (long long)time(nullptr);
	for(const MpInvite &v : mInvites)
		if(v.id == id && v.uses > 0 && (v.expires == 0 || v.expires > now))
			return &v;
	return nullptr;
}

void
MpAccounts::useInvite(const std::string &id)
{
	for(MpInvite &v : mInvites)
		if(v.id == id)
			v.uses--;
	saveInvites();
}

void
MpAccounts::madeUpSalt(const std::string &name, uint8_t salt[MP_SALT_LEN]) const
{
	std::string lower;
	for(unsigned char ch : name)
		lower += (char)tolower(ch);
	crypto_blake2b_keyed(salt, MP_SALT_LEN, mSecret, sizeof(mSecret), (const uint8_t*)lower.data(), lower.size());
}

void
MpAccounts::sign(const std::string &message, uint8_t signature[64]) const
{
	crypto_eddsa_sign(signature, mIdentitySecret, (const uint8_t*)message.data(), message.size());
}

static std::string
roomFile(const std::string &folder, const std::string &room)
{
	return folder + "/" + room + ".meta";
}

MpRoomSettings
MpAccounts::room(const std::string &room) const
{
	MpRoomSettings settings;
	std::string text;
	if(!readWhole(roomFile(roomsFolder(), room), &text))
		return settings;
	for(const std::string &line : lines(text)){
		std::vector<std::string> f = fields(line);
		if(f[0] == "lock" && f.size() >= 3)
			settings.locked = fromHex(f[1], settings.lockSalt, MP_SALT_LEN) && fromHex(f[2], settings.lockKey, MP_KEY_LEN);
		else if(f[0] == "role" && f.size() >= 3 && MpParseRole(f[2]) != MP_ROLE_NONE)
			settings.roles[f[1]] = MpParseRole(f[2]);
		else if(f[0] == "agents" && f.size() >= 2)
			settings.agentRole = f[1] == "none" ? MP_ROLE_NONE : MpParseRole(f[1]);
	}
	return settings;
}

bool
MpAccounts::saveRoom(const std::string &room, const MpRoomSettings &settings)
{
	std::string out = "# haumea-server room settings: lock, roles in it, what agents may do.\n";
	if(settings.locked)
		out += "lock\t" + toHex(settings.lockSalt, MP_SALT_LEN) + "\t" + toHex(settings.lockKey, MP_KEY_LEN) + "\n";
	for(const auto &role : settings.roles)
		out += "role\t" + role.first + "\t" + MpRoleName(role.second) + "\n";
	out += std::string("agents\t") + (settings.agentRole == MP_ROLE_NONE ? "none" : MpRoleName(settings.agentRole)) + "\n";
	return writePrivate(roomFile(roomsFolder(), room), out);
}

void
MpAccounts::forgetRoom(const std::string &room)
{
	::remove(roomFile(roomsFolder(), room).c_str());
}
