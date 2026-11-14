# Wake word and battery operation

## Power states

| State | What runs | Rough draw* |
|---|---|---|
| **Listening** | CPU + PSRAM + ES7210 mics + wake word model. Wi-Fi **off**. | ~60–120 mA |
| **Connecting** | Wi-Fi association + TLS + Gemini setup (~1–3 s). Speech after the wake word is buffered. | ~150–300 mA peaks |
| **Conversation** | Wi-Fi streaming both ways + speaker amp | ~200–500 mA (speaker volume dependent) |
| **Deep sleep** (optional, button wake only) | RTC only | well under 1 mA |

*Estimates, to be replaced with measurements on the real board.

A wake word needs the CPU and microphones running continuously, so the device
**cannot deep-sleep while listening**. Expect roughly a day per ~2000 mAh cell when idle
listening, less with heavy use. Use cases needing weeks of standby should use a button
(deep sleep + GPIO wake) instead of, or in addition to, the wake word.

## Flow

```
listening (Wi-Fi off) --wake word--> chime, start buffering mic audio
   -> Wi-Fi fast reconnect (stored BSSID/channel, static IP optional)
   -> open Gemini session (setup + appointment summary)
   -> flush buffered audio, then stream live
conversation --CONFIG_VA_IDLE_TIMEOUT_S silence--> close session -> Wi-Fi off -> listening
```

Reminders: a timer wakes the conversation path without a wake word.
The PCF85063 RTC keeps time across resets; SNTP corrects it when Wi-Fi is up.

## Wake word engine (to decide)

| | ESP-SR WakeNet | microWakeWord |
|---|---|---|
| Ready-made words | Several (e.g. "Hi ESP", "Alexa", "Jarvis") | "Okay Nabu", "Hey Jarvis", "Hey Mycroft", "Alexa" |
| Custom word | Via Espressif (training service) | Train yourself (open-source pipeline) |
| Echo cancellation | Bundled AFE (AEC, noise suppression, VAD) | Separate (can still use ESP-SR AFE) |
| License | Espressif license (free on Espressif chips) | Apache-2.0 |
