# Direct Hermes integration

The intended direction is **browser PWA / puck → voice gateway → Hermes agent**,
without Home Assistant in the path. This is an integration plan, not a claim
that the public quick start already runs a full Hermes agent.

The product goal is one Hermes voice agent across phone and puck, with actions
the user can see and control. That includes deliberate cross-device handoff,
visible approvals for consequential actions, understandable privacy controls,
and a guided setup. Existing Stop, mute, reset and export controls are a starting
point; they are not a completed agent-action approval or shared-session system.

## What the public source does today

The standalone [server](../server.py) explicitly clears `VOICE_AGENT_SOCKET`
and `VOICE_HERMES_SOCKET` from its inherited environment and selects
[`OpenAIChat`](../services/voice-gateway/openai_adapter.py). That path supplies
bounded text completion from the configured OpenAI-compatible endpoint; it
does not expose agent tools, filesystem access or a Hermes profile.

Two transport adapters are retained in source:

- [`UnixHermes`](../services/voice-gateway/hermes_adapter.py): length-prefixed
  text over a local Unix socket, with bounded response size and timeout.
- [`UnixAgent`](../services/voice-gateway/agent_adapter.py): a versioned,
  identity-bound request/response contract with authenticated turn context and
  validated tool traces. Its [contract](../services/voice-gateway/agent_contract.py)
  distinguishes guarded read-only tools from configured profile tools.

These are application-specific socket protocols, not evidence that an arbitrary
Hermes installation already exposes a compatible endpoint. The current Compose
file neither runs a Hermes agent nor mounts an agent socket. Setting an
environment variable alone does not activate this integration in the standalone
server.

## Implementation and acceptance scope

1. Pin the supported Hermes revision and define the actual server-side agent
   entrypoint. Supply a compatible connector service or explicitly adapt the
   transport to the verified Hermes interface.
2. Add an explicit opt-in backend mode to setup, server startup and health
   checks. Keep the current chat backend available and reject missing or
   incompatible agent configuration instead of silently falling back.
3. Bind authenticated browser/puck identities to the intended Hermes principal,
   session and profile on the server. Implement explicit phone/puck conversation
   handoff and define context/reset ownership. Prove that one device or profile
   cannot receive another's conversation; do not silently merge today's separate
   contexts.
4. Make tool scope, permissions and consequential-action approval explicit in
   both the phone and puck UI, including the proposed action and final outcome.
   Voice or typed text must not bypass the agent's normal safeguards. Keep
   secrets and tool execution off the browser and puck.
5. Propagate Stop/cancellation to the real agent, suppress late speech, bound
   concurrency and output, and test reconnect, timeout, reset and restart.
6. Test the complete PWA → gateway → real Hermes → speech path, then the actual
   puck with the exact firmware/model/board combination. Record versions and
   distinguish fixtures from real-model and physical-device evidence.
7. Add a guided setup that checks the pinned Hermes service, trusted gateway,
   models, selected device and permission scope before declaring readiness.

This work does not require Home Assistant. It also does not automatically
provide its integrations, share browser/puck contexts, or validate hardware
charging, update recovery or soak gates. Those are separate decisions and checks.

See [current architecture](ARCHITECTURE.md), [installation](INSTALL.md), and
[hardware acceptance](HARDWARE.md).
