# cJSON (vendored for host tests only)

The firmware uses ESP-IDF's bundled cJSON. The host unit tests (`tests/host`) build
without ESP-IDF, so they use this copy of the same library.

- Upstream: https://github.com/DaveGamble/cJSON
- Version: v1.7.18 (commit acc76239bee01d8e9c858ae2cab296704e52d916)
- License: MIT (see LICENSE)
- SHA-256:
  - `cJSON.c` 75c51de8fa40ac9d7a99319c6330719bd692eb81c0a869265f3d4c682533f9b9
  - `cJSON.h` 0578cc29132912edbc88f83207a8fc76e5db3db0605497e909a9384ef3cc474b

Do not edit these files; update by replacing them with a newer upstream release.
