# Digimon 0.2.0 - build and verification

Prepared 17 October 2026. The ESP32-S3 firmware builds with ESP-IDF 5.2.3 and Espressif GCC 13.2.0. It fits the existing 0x3f0000-byte app slot. `firmware/manifest.json` records exact binary hashes and `build_metadata.json` records the compiled source hashes.

Passing local checks:

- Production audio: bounded WAV capture, pre-roll, speech/silence limits, manual/mute isolation, PCM volume and mouth amplitude, HTTPS framing and cancellation.
- Controller: capture/cancel races, stale replies, semantic acceptance, ambient/uncertain rejection, follow-up expiry, manual bypass, UTF-8 context boundaries and physical mute persistence failures.
- Buttons/UI: debouncing, tap/hold/chords, hold-to-scroll, timer wrap, all printable password characters, input bounds, secret wiping, scanned-network selection and face animation. `preview.png` was rendered from the actual drawing functions and visually inspected.
- Intent/preferences: strict Responses request/output schema, quiet failures, no acceptance after mute/cancel, atomic persistence, safe corruption recovery and physical-only unmute.
- MCP/tunnel: discovery, four tools, event subscriptions, signatures, persistence, bounded callback delivery, authentication and reply correlation. Ten generated core request/response fixtures pass the official MCP 2.0 JSON schema.
- Clock/config/Wi-Fi: dates, credential bounds, scan-only initial setup, bounded sorted scans, failure recovery and reconnect isolation.
- Setup/build/flash tools: mocked serial provisioning, five packaging tests, six backup/write-guard tests and offline bundle hash validation. Tests do not write device flash.

The SDK is isolated at commit `c9763f62dd00c887a1a8fafe388db868a7e44069`. Its three compiler-path assignments use existing Windows short directory names while retaining full executable filenames. The build wrapper does not modify an SDK. Fresh SDK/toolchain installations should use paths without spaces.

## Hardware verification

The attached board has been identified on COM6. The first automatic bootloader entry and a high-speed full-flash read failed; no flash writes occurred during those attempts. The documented physical BOOT/power-cycle sequence established a downloader connection. A slower checkpointed full backup is being verified before the first firmware write. Flash/readback/boot results will be added here after they have actually completed.

Live OpenAI credentials and a ChatGPT dot subscription have not been provisioned. Natural addressing accuracy, the complete spoken round trip and battery life remain unverified. The local energy detector and cloud address classifier are experimental and do not identify the speaker.
