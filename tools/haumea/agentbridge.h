// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// Native, main-thread control primitives for local visual agent loops. The
// engine owns scene safety and rendering; CLI/MCP concerns stay out-of-process.
void AgentBridgeUpdate(void);
bool AgentBridgeWorkerResting(void);	// --background, idle: this frame draws nothing
// An agent edit session is open (begin_edit_session until commit or rollback).
bool AgentBridgeSessionActive(void);
void AgentBridgeCaptureAfterWorldRender(void);
void AgentBridgeCaptureAfterUiRender(void);	// capture_screen: with the panels
// The socket this editor listens on for Namaka, or "" (not listening, or TCP).
const char *AgentBridgeSocketPath(void);
// Another process (another Haumea?) answered on the socket, so this one isn't listening.
bool AgentBridgeSocketBusy(void);
// Listening for an agent, on a socket or loopback TCP.
bool AgentBridgeListening(void);
