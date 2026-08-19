# Cactus Cam — product notes

## What it is

A camera you talk to. The XIAO ESP32S3 Sense becomes an agent with a Telegram
identity: it receives plain-words commands, acknowledges instantly, does the
work while editing its own status message, and reports back. It also runs the
barkcam pipeline (PDM mic → energy detector → photo alert) so it doubles as a
bark camera.

## Design decisions (and why)

**Deterministic parser, no LLM.** Commands are keyword matches with numeric
clamps. Every reply is predictable; nothing can hang on a model call or burn
tokens. The "agent" feel comes from the ack/progress/edit choreography, not
from a model.

**Instant ack first.** The poll task pushes updates onto a queue; `loop()`
drains one per pass so the mic and detector are never starved. The ack goes
out before any work starts — the user always knows it was heard ("needle-ing
the cactus…").

**Video = frames, not encoding.** The ESP32 has no video encoder. A clip is N
JPEG frames captured in the existing JPEG mode, streamed to the studio service
as a chunked HTTP body (bounded memory: one frame buffer), and ffmpeg turns it
into an MP4 that gets posted back. No camera re-init, no MJPEG sensor mode.

**Audio = WAV first.** 16 kHz mono PCM from the PDM mic, teed off the same
samples the detector consumes (no second I2S reader). 44-byte header + PCM →
`sendAudio`. OGG/opus is a later polish, not a v1 requirement.

**It knows its limits.** Every action has clamps (video 2–15 s, voice 2–20 s,
timers ≥10 s photo / ≥20 s video, ≤3 timers, 1 h max interval) and refuses in
plain language instead of crashing. A watchdog task reboots a stalled loop; a
6 h periodic reboot keeps unattended duty clean.

**Config AP with web UI.** First 10 minutes after power-on (or when WiFi is
lost): open AP `cactuscam-config`, single-page UI at `cactuscam.local` —
same design as barkcam. Closes when the phone leaves.

**One source of truth for credentials.** `fw/include/credentials.h` is
gitignored; the studio service reads the same file, so bot token and chat ID
live in exactly one place.

## Known limits (v1)

- Clips need the studio machine awake and on the LAN; without it, video
  commands fail gracefully (photos/voice still work).
- `STUDIO_HOST` is a compile-time constant — IP change means reflash.
- One user: the poller accepts messages from the configured chat ID only.
- Sharpness/denoise don't exist on the OV2640 (driver no-ops) — not exposed.

## Roadmap candidates

OGG/opus voice notes · studio auto-discovery (mDNS) instead of fixed IP ·
clip length in the web UI · per-timer on/off from Telegram · night-vision
preset (manual AEC + gain ceiling) as a one-word command.
