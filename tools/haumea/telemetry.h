// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <string>

// The daily usage ping, opt-in. Nothing is sent and no install ID exists
// until the user says yes (the start screen asks; leaving it unanswered is
// no; Preferences > Privacy changes it). Then, at most once a UTC day, one
// POST of {uid, version, channel, os} to
// https://telemetry.itsneufox.xyz/haumea/telemetry/ping; a 2xx answer marks
// the day sent, anything else is dropped and not retried. Never in tests,
// UI scripts, command-line modes, agent sessions or CI, nor with
// HAUMEA_NO_TELEMETRY; HAUMEA_TELEMETRY_URL points it at a test server
// (loopback may be http) and lets an automated run send.
// What it sends and keeps: tools/telemetry/privacy.html, docs/USER_GUIDE.md.

enum TelemetryConsent
{
	TELEMETRY_UNASKED = -1,
	TELEMETRY_NO = 0,
	TELEMETRY_YES = 1,
};

int TelemetryGetConsent(void);
bool TelemetryIsEnabled(void);
// Yes makes the install ID and sends today's ping; no deletes the ID.
void TelemetrySetEnabled(bool enabled);

// At start: today's ping, when it is on and allowed (non-blocking).
void TelemetrySendPing(void);
// Every frame: notes a finished ping (a 2xx marks the day sent).
void TelemetryUpdate(void);

// The start screen's question, drawn while unanswered.
void TelemetryDrawConsent(void);
// The start screen closed: an unanswered question counts as no.
void TelemetryStartScreenClosed(void);
// Preferences > Privacy.
void TelemetryPreferences(void);

// For crash reports: the install ID while the ping is on (else ""), and the
// same POST: a JSON body to an https address (or a loopback http one when
// testing); the HTTP status, 0 without an answer. Blocking.
std::string TelemetryInstallId(void);
int TelemetryPostJson(const std::string &url, const std::string &body);
bool TelemetryUrlAllowed(const std::string &url, bool isTestUrl);
