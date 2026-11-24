# esp32-voice-assistant

**[Install or update the firmware in your browser →](https://schwer.github.io/esp32-voice-assistant/)**
(Chrome or Edge on a computer, board connected by USB)

An open-source, voice-only AI assistant for ESP32-S3 audio boards. It streams audio
directly to a realtime speech-to-speech API (Gemini Live today) over a TLS WebSocket.
There's no phone app and no cloud backend of your own.

> **Status:** early, under active development. Wake word, speaker, status messages, setup and
> diagnostics pages work; Gemini voice conversations are new and still being tested on hardware.

## Features

- Natural voice conversation; **interrupt him** by just talking (with an echo guard that pauses
  interruptions if he starts hearing himself)
- **Only answers when spoken to**: ignores the TV, background noise and people talking to each
  other (Gemini proactive audio, turned off automatically if the model doesn't support it)
- Echo cancellation via Espressif AFE
- **On-device memory**: appointments and notes stored on flash, exposed to the model as tools
- Spoken reminders, triggered by the device itself
- **Battery questions** ("How's your battery?", "Are you charging?", "How long will you last?")
- **Timers, alarms and a stopwatch**, kept on the board so they work offline: "set a pasta timer
  for 10 minutes", "wake me at 7:30 on weekdays", "start the stopwatch", "how long is left?".
  When one goes off the board says so, beeps and pulses amber; say "Jarvis" (or press Stop
  ringing on the diagnostics page) to silence it. Alarms survive restarts; timers don't.
- **Weather** from Open-Meteo (free, no key) for your home location or anywhere you name
- **Google Search** grounding for current information (when the model supports it)
- **Saved conversations**: say "remember this conversation", later "let's resume our conversation",
  hear the saved ones newest first and say "that one"
- **Wi-Fi anywhere**: remembers up to 5 networks (home, your phone's hotspot, work) and joins
  whichever is around. Optionally uses free Wi-Fi with no password, but only if it really reaches
  the internet (networks that want you to click "Accept" on a page are skipped), and the setup page
  switches itself off on public networks.
- **Intercom**: on the board's Intercom page (linked from Diagnostics), type a message and Jarvis
  announces it after a chime, in his Gemini voice. The last 10 are kept as audio, so **Say again**
  replays one instantly, even without Gemini. "Jarvis, what was the last message?" works too.
- **Updates by voice**: "Jarvis, is there an update?" / "Jarvis, update yourself". It only ever
  installs when you ask (or press Install update on the diagnostics page) and says when it starts,
  when it's done, or that it didn't work. It mentions a new version once, at the end of an answer.
  See [Updates over Wi-Fi](#updates-over-wi-fi).
- **Battery saver**: on battery, Wi-Fi turns off after 3 quiet minutes and back on at "Jarvis"
  (your first words are buffered meanwhile); the radio dozes whenever idle. See [docs/power.md](docs/power.md).
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
| `components/net` | Wi-Fi: saved networks, free open networks (with an internet check), power on/off |
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

**One voice.** When you save the setup page with a new voice (or the first time you save your
API key), the board immediately records all of its own lines (wake-word answers, status
messages) by asking Gemini Live to read them in that voice, about a minute, well under a cent.
The setup page shows a progress bar and tells you when it's finished; then messages and
conversations switch to the new voice together, and you can play every message on the
diagnostics page. Until the first recording, the built-in clips are used.

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

1. Open the [web installer](https://schwer.github.io/esp32-voice-assistant/) in desktop Chrome or Edge.
2. Connect the board over USB and click **Install**. The latest firmware installs automatically;
   updates keep your Wi-Fi and settings.
3. Pick your Wi-Fi network from the list the board sees (2.4 GHz) and enter the password.
   Add more networks (e.g. your phone's hotspot) later with **Add Wi-Fi network** here or on the
   board's setup page.
4. Open the setup page link shown by the installer and paste your
   [Gemini API key](https://aistudio.google.com/apikey).
5. Say **"Jarvis"**.

## Building

Requires ESP-IDF 5.5+.

```bash
idf.py set-target esp32s3
idf.py menuconfig      # Voice Assistant → timezone, API key / token URL (dev builds only)
idf.py build flash monitor
```

## Testing

CI (`.github/workflows/test.yml`) runs on every push and pull request:

| Suite | What it covers | Run locally |
|---|---|---|
| Host unit tests (C) | Audio maths (resampler, mu-law, speaker processing, trimming), battery estimator, Improv protocol, setup-page helpers, alarm scheduling (repeats, daylight saving) and stopwatch, and Gemini Live messages: what the firmware sends, plus whole server conversations replayed from `tests/host/fixtures/gemini` (hand-written in the API's message shapes; no API key needed). Built with AddressSanitizer + UBSan. | `make -C tests/host` |
| Tool checks (Python) | Firmware parts never overlap NVS, manifest format, partition table, phrase list / enum / clips stay in sync | `python -m pytest tests/tools` |
| Browser tests (Playwright) | Installer against a simulated board (Improv over Web Serial), diagnostics page, setup page's recording progress | `cd tests/web && npm ci && npx playwright install chromium && npx playwright test` |

The firmware build (`build.yml`) is the compile check. Hardware behaviour (audio, wake word,
battery readings) still needs a board: use the diagnostics page's tests.

Hardware-independent code lives in modules without ESP-IDF dependencies so it can be tested
on a normal computer: `components/dsp`, `components/battery/battery_est.c`,
`components/improv/improv_proto.c`, `components/setup_web/web_util.c`,
`components/gemini_live/gemini_proto.c`, `components/clock/clock_logic.c`, `components/net/net_logic.c`.

## Security notes

- Embedding an API key in firmware is for **prototyping only**. Use ephemeral tokens
  (`CONFIG_VA_TOKEN_URL`) for anything you share.
- Enable flash encryption and Secure Boot before storing any account credentials.

## License

MIT. See [LICENSE](LICENSE).

Not affiliated with or endorsed by Google. "Gemini" is a trademark of Google LLC.

## Updates over Wi-Fi

Say "Jarvis, update yourself" (he checks, tells you the new version and asks before installing) or
use **Check for updates** / **Install update** on the diagnostics page. The board never installs
anything on its own: it only checks twice a day, and mentions a new version once.

Before installing, Jarvis says what's new: the top three lines of [`whats_new.txt`](whats_new.txt)
(Gemini puts them in its own words; without Gemini the board plays a recording of them made when the
installer page is published). The diagnostics page shows them too. **Add a line to `whats_new.txt`
for every change people would notice.**

Without Gemini (no credit or no key) it still works: when "Jarvis" can't reach Gemini and an update
is waiting, the board says so and asks you to say "Jarvis" again to install it.

How it's kept safe:

- It downloads only from the HTTPS site built into the firmware (`CONFIG_VA_UPDATE_URL`, this
  project's GitHub Pages site), with the certificate checked, and only files next to its manifest.
- The download must have exactly the size and SHA-256 listed in the manifest, be this project's
  firmware for the ESP32-S3, and pass the chip's own image checks. Older versions are never installed.
- The new version runs on trial: if it crashes, or can't reach Wi-Fi within 5 minutes, the board goes
  back to the previous version by itself and says the update didn't work.
- It won't start on a low battery (below 25% and not charging).

Not covered: the images aren't cryptographically signed, so the update is only as trustworthy as
this GitHub repository and its Pages site (see the supply-chain issue).

The first time, install from the web page with a USB cable: that sets up the two firmware slots
updates need. Wi-Fi, settings, recordings, conversations and alarms are kept. If a later version
changes the memory layout or the "Jarvis" model, the board says it needs the web page again.
