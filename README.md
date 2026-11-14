# esp32-voice-assistant

An open-source, voice-only AI assistant for ESP32-S3 audio boards. It streams audio
directly to a realtime speech-to-speech API (Gemini Live today) over a TLS WebSocket.
There's no phone app and no cloud backend of your own.

> **Status:** early, under active development. Wake word, speaker, status messages, setup and
> diagnostics pages work; Gemini voice conversations are new and still being tested on hardware.

## Features

- Natural voice conversation with barge-in (server-side VAD)
- Echo cancellation via Espressif AFE
- **On-device memory**: appointments and notes stored on flash, exposed to the model as tools
- Spoken reminders, triggered by the device itself
- **Weather** from Open-Meteo (free, no key) for your home location or anywhere you name
- **Google Search** grounding for current information (when the model supports it)
- **Saved conversations**: say "remember this conversation", later "let's resume our conversation",
  hear the saved ones newest first and say "that one"
- Wake word with Wi-Fi off between conversations, for battery operation. See [docs/power.md](docs/power.md).
- Local setup page at `http://assistant.local`, with a diagnostics page at `/diag`
- `.ics` export of appointments
- Optional account integrations (Google Calendar, IMAP, CalDAV, Outlook), authorized on your LAN.
  See [docs/integrations.md](docs/integrations.md).

## Architecture

```
mic → I2S → AFE (AEC/VAD) → 16 kHz PCM ──────────┐
                                                 ├─ wss:// Gemini Live (BidiGenerateContent)
speaker ← I2S ← jitter buffer ← 24→16 kHz resample ─┘
                                 toolCall → tools → appointments (LittleFS)
```

| Component | Responsibility |
|---|---|
| `components/board` | Pin map and I2C addresses for the target board |
| `components/wakeword` | Always-on wake word detection |
| `components/net` | Wi-Fi station, credentials in NVS, power on/off |
| `components/improv` | Improv Wi-Fi over serial for the web flasher |
| `components/leds` | LED ring status lights (legend and previews on `/diag`) |
| `components/storage` | LittleFS on the `storage` partition at `/data` |
| `components/conversations` | Saved conversations: rolling transcript, save/list/load/delete |
| `components/diag` | Diagnostics: log capture, health snapshot, API key check (`/diag`) |
| `components/prompts` | Pre-recorded spoken status messages (offline) |
| `components/audio` | Codec, I2S capture/playback, AEC, playback flush |
| `components/gemini_live` | WebSocket session, setup, audio streaming, tool calls, resumption |
| `components/tools` | Function declarations + dispatch |
| `components/appointments` | Persistent store, summaries, reminders |
| `components/setup_web` | Local setup UI, `.ics` export, future OAuth callback |

## Lights and spoken status

When you pick a different voice on the setup page, the device records its own lines in that
voice by asking Gemini Live to read them (well under a cent), so the wake-word answer, status
messages and conversations all sound like one assistant. Otherwise it uses built-in clips.

The device also says what's happening ("I need Wi-Fi", "Ready. Say Jarvis when you need me.",
"Settings saved", ...). Turn this off on the setup page. Phrases live in `tools/prompts.tsv`;
CI regenerates the clips with Piper TTS when that file changes.

| Pattern | Meaning |
|---|---|
| Off | Idle, waiting for "Jarvis" |
| Quick white flash | Heard "Jarvis" (it also answers "Yes?", "I'm listening." or "How can I help?") |
| Blue pulse | Starting up or connecting |
| Rainbow cycle | Ready for your voice |
| Purple spinner | Working on your request |
| Soft white glow | Assistant is talking |
| 2 green blinks | Saved (conversation or settings) |
| Orange sweep | Deleted |
| Amber blink every few seconds | Wi-Fi or API key missing |
| Amber breathing | A reminder is due |
| 3 red blinks | Something went wrong (see `/diag`) |

## Hardware

- Reference board: **Waveshare ESP32-S3-AUDIO-Board** (ESP32-S3R8, ES8311 + ES7210, dual mics, battery header)
- Other ESP32-S3 boards with **8 MB PSRAM** need a `components/board/include/board.h` for their pins
- 3.7 V LiPo on the MX1.25 header for battery use

## Installing

Open the web flasher (GitHub Pages for this repo) in desktop Chrome or Edge, connect the
board over USB and click **Install**. You'll be asked for your Wi-Fi network right after
flashing (Improv Wi-Fi). Then open the device's setup page to enter your API key.

## Building

Requires ESP-IDF 5.5+.

```bash
idf.py set-target esp32s3
idf.py menuconfig      # Voice Assistant → timezone, API key / token URL (dev builds only)
idf.py build flash monitor
```

## Security notes

- Embedding an API key in firmware is for **prototyping only**. Use ephemeral tokens
  (`CONFIG_VA_TOKEN_URL`) for anything you share.
- Enable flash encryption and Secure Boot before storing any account credentials.

## License

MIT. See [LICENSE](LICENSE).

Not affiliated with or endorsed by Google. "Gemini" is a trademark of Google LLC.
