// SPDX-License-Identifier: GPL-2.0-or-later
// haumea-server's accounts, invites, room settings and identity (mprelay.cpp
// uses them when it has a state folder). Part of the multiplayer core that
// haumea-server owns and Haumea keeps a copy of.
//
// In the state folder:
//   accounts.txt   name, role, password salt and key (Argon2id, as the handshake uses)
//   invites.txt    invite id, role, salt and key of its secret, uses left, expiry, who made it
//   identity.key   the server's Ed25519 key: editors remember it, so a server in the middle shows
//   secret.key     for the made-up salt of a name that has no account (no telling which exist)
//   rooms/<room>.meta  its lock, roles in it and what agents may do there
#pragma once
#include "mpprotocol.h"

#include <map>
#include <string>
#include <vector>

enum MpRole {
	MP_ROLE_NONE = 0,
	MP_ROLE_VIEWER,	// looks and chats
	MP_ROLE_BUILDER,	// edits, saves
	MP_ROLE_ADMIN,	// invites, sets roles below its own, kicks, makes and locks rooms, loads maps
	MP_ROLE_OWNER,	// all of it, admins too
};
const char *MpRoleName(int role);	// "viewer"... "" for none
int MpParseRole(const std::string &name);	// MP_ROLE_NONE if it isn't one

struct MpAccount
{
	std::string name;
	int role = MP_ROLE_NONE;
	uint8_t salt[MP_SALT_LEN] = {}, key[MP_KEY_LEN] = {};
};

struct MpInvite
{
	std::string id;	// what the editor sends in the clear; the secret after the dash proves it
	int role = MP_ROLE_BUILDER;
	uint8_t salt[MP_SALT_LEN] = {}, key[MP_KEY_LEN] = {};
	int uses = 1;
	long long expires = 0;	// unix time; 0 never
	std::string by;
};

struct MpRoomSettings
{
	bool locked = false;
	uint8_t lockSalt[MP_SALT_LEN] = {}, lockKey[MP_KEY_LEN] = {};
	std::map<std::string, int> roles;	// an account's role in this room, over its own
	int agentRole = MP_ROLE_BUILDER;	// the most an agent may do here; MP_ROLE_NONE keeps them out
};

class MpAccounts
{
public:
	bool open(const std::string &folder, std::string *error);
	bool isOpen(void) const { return !mFolder.empty(); }
	const std::string &folder(void) const { return mFolder; }
	// Edits made by haumea-server's commands while it runs come in.
	void reloadIfChanged(void);

	const MpAccount *find(const std::string &name) const;
	bool nameTaken(const std::string &name) const;
	static bool validName(const std::string &name);
	bool create(const std::string &name, const std::string &password, int role, std::string *error);
	bool setRole(const std::string &name, int role);
	bool setPassword(const std::string &name, const std::string &password);
	bool remove(const std::string &name);
	std::vector<MpAccount> all(void) const;

	// A new invite: "ID-SECRET", shown once.
	std::string invite(int role, int uses, int days, const std::string &by);
	const MpInvite *findInvite(const std::string &id) const;
	void useInvite(const std::string &id);

	// A name without an account gets a salt all the same, the same each time.
	void madeUpSalt(const std::string &name, uint8_t salt[MP_SALT_LEN]) const;

	const uint8_t *identityPublic(void) const { return mIdentityPublic; }
	void sign(const std::string &message, uint8_t signature[64]) const;

	MpRoomSettings room(const std::string &room) const;
	bool saveRoom(const std::string &room, const MpRoomSettings &settings);
	void forgetRoom(const std::string &room);
	std::string roomsFolder(void) const { return mFolder + "/rooms"; }

private:
	bool load(std::string *error);
	bool saveAccounts(void);
	bool saveInvites(void);
	long long stamps(void) const;

	std::string mFolder;
	std::vector<MpAccount> mAccounts;
	std::vector<MpInvite> mInvites;
	uint8_t mSecret[32] = {};
	uint8_t mIdentitySecret[64] = {}, mIdentityPublic[32] = {};
	long long mLoaded = 0;	// the files' times when last read
};

// The server's fingerprint as people compare it: its identity key, short hex.
std::string MpFingerprint(const uint8_t publicKey[32]);
