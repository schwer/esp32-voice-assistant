# esp32-voice-assistant

An open-source, voice-only AI assistant for ESP32-S3 audio boards. It streams audio
directly to a realtime speech-to-speech API (Gemini Live today) over a TLS WebSocket.
There's no phone app and no cloud backend of your own.

> **Status:** early scaffolding. Not functional yet.

## Features (planned)

- Natural voice conversation with barge-in (server-side VAD)
- Echo cancellation via Espressif AFE
- **On-device memory**: appointments and notes stored on flash, exposed to the model as tools
- Spoken reminders, triggered by the device itself
- Local setup page at `http://assistant.local`
- `.ics` export of appointments
- Optional account integrations (Google Calendar, IMAP, CalDAV, Outlook), authorized on your LAN.
  See [docs/integrations.md](docs/integrations.md).

## Architecture

```
mic → I2S → AFE (AEC/VAD) → 16 kHz PCM ─┐
                                        ├─ wss:// Gemini Live (BidiGenerateContent)
speaker ← I2S ← jitter buffer ← 24 kHz ─┘
                                 toolCall → tools → appointments (LittleFS)
```

| Component | Responsibility |
|---|---|
| `components/audio` | Codec, I2S capture/playback, AEC, playback flush |
| `components/gemini_live` | WebSocket session, setup, audio streaming, tool calls, resumption |
| `components/tools` | Function declarations + dispatch |
| `components/appointments` | Persistent store, summaries, reminders |
| `components/setup_web` | Local setup UI, `.ics` export, future OAuth callback |

## Hardware

- ESP32-S3 with **8 MB PSRAM** (e.g. N16R8 modules)
- I2S audio codec with mic + speaker amp (ESP32-S3-Box-3, Korvo-2, or a custom board)

## Building

Requires ESP-IDF 5.3+.

```bash
idf.py set-target esp32s3
idf.py menuconfig      # Voice Assistant → Wi-Fi, timezone, API key / token URL
idf.py build flash monitor
```

## Security notes

- Embedding an API key in firmware is for **prototyping only**. Use ephemeral tokens
  (`CONFIG_VA_TOKEN_URL`) for anything you share.
- Enable flash encryption and Secure Boot before storing any account credentials.

## License

MIT. See [LICENSE](LICENSE).

Not affiliated with or endorsed by Google. "Gemini" is a trademark of Google LLC.
