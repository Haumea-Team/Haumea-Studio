// SPDX-License-Identifier: GPL-2.0-or-later
#include "haumea.h"
#include "discordpresence.h"
#include "discordrpc.h"
#include "workspace.h"
#include "pawneditor.h"
#include "textdraws.h"
#include "i18n.h"
#include <chrono>
#include <exception>

namespace
{
bool enabled = true;
std::unique_ptr<DiscordRPC::Client> client;
// Every automatic test/CLI run stays off the real user's Discord. Tests of
// the protocol construct the independent client against private IPC sockets.
bool allowed()
{
	return !IsAutomatedRun() && !getenv("HAUMEA_NO_DISCORD");
}
} // namespace
void DiscordPresenceTick()
{
	if (!allowed())
		return;
	static auto retry = std::chrono::steady_clock::time_point{};
	if (enabled && !client && std::chrono::steady_clock::now() >= retry) {
		try {
			client.reset(new DiscordRPC::Client);
		} catch (const std::exception &error) {
			log("Discord presence: %s\n", error.what());
			retry = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		}
	}
	if (client)
		client->Update(enabled, InPawnIdeMode()	   ? DiscordRPC::Workspace::Code
					: InTextDrawMode() ? DiscordRPC::Workspace::TextDraws
							   : DiscordRPC::Workspace::Map);
}
void DiscordPresenceShutdown()
{
	client.reset();
}
bool DiscordPresencePreferences()
{
	ImGui::SeparatorText(_("Discord"));
	bool changed = UiCheckbox(_("Show activity on Discord"), &enabled);
	ImGui::SetItemTooltip(_("Shows the active workspace and session time through Discord desktop.\nMap names, "
				"filenames and project paths are never shared."));
	if (changed)
		DiscordPresenceTick();
	if (enabled) {
		ImGui::TextDisabled("%s", client && client->GetStatus() == DiscordRPC::Status::Connected
					      ? _("Connected to Discord")
					      : _("Waiting for Discord desktop"));
	}
	return changed;
}
bool DiscordPresenceLoadSetting(const char *key, const char *value)
{
	if (strcmp(key, "discord_presence"))
		return false;
	// Settings use the same 0/1 convention as the other privacy preferences.
	char *end = nullptr;
	long n = strtol(value, &end, 10);
	if (end != value && (n == 0 || n == 1))
		enabled = n != 0;
	return true;
}
void DiscordPresenceSaveSettings(FILE *file)
{
	fprintf(file, "discord_presence %d\n", enabled ? 1 : 0);
}
