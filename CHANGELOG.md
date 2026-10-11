# Changelog

The firmware version is `FIRMWARE_VERSION` in `main/opcua_server.c`, readable over
OPC UA as **M5Core2 → System → FirmwareVersion**.

## 1.1.0 (2026-10-11)

### Added
- **Optional certificate security.** Switched on and off on the Core2 screen
  (System page → Security), saved in flash, applied by a restart. When on, the
  server offers None plus Sign and Sign & Encrypt for Basic256Sha256,
  Aes128_Sha256_RsaOaep and Aes256_Sha256_RsaPss. Login stays anonymous.
  - The Core2 creates its own certificate (RSA 2048, 10 years) the first time,
    in about 5 seconds, and keeps it in flash.
  - Unknown client certificates are refused, and the Core2 screen asks
    "Trust this client?". Trusted certificates are saved in flash (up to 16)
    and can be cleared from the Security menu.
  - `System.SecurityEnabled` shows whether security is on.
- **Device ID** (last 3 bytes of the MAC) for running several Core2s on one
  network: the hostname becomes `m5core2-<id>` and the ApplicationUri
  `urn:m5stack:core2:opcua:<id>:server`. Shown on the System page and as
  `System.DeviceId`. The node namespace (`urn:m5stack:core2:opcua`) is the same on
  every device, so client configurations carry over.

### Changed
- open62541 regenerated with mbedTLS encryption (`UA_ENABLE_ENCRYPTION=MBEDTLS`);
  otherwise the same options. The clock patch is applied as before.
- open62541 and mbedTLS now allocate from PSRAM, which frees about 100 kB of
  internal RAM for Wi-Fi and sockets.
- New flash partition `certs` for the security data. A full flash (`idf.py flash`)
  writes the new partition table.
- Hostname and ApplicationUri changed (see Device ID): clients that saved the old
  ApplicationUri, or trusted the old server certificate, need to accept the new one once.

## 1.0.1 (2026-10-09)

### Fixed
- **Strict OPC UA clients (ABB 800xA) rejected standard nodes with a null data type.**
  The bundled open62541 was generated with XML encoding off, which leaves 30
  namespace-0 values empty and untyped, starting with `i=12874` (InputArguments of
  `Server.ResendData`). The new `main/ns0_fixups.c` fills them in at startup:
  - 15 values that should have come from the generated XML get their exact standard
    values (method arguments of ResendData and GetMonitoredItems, EnumStrings and
    EnumValues, NamingRule, two type descriptions)
  - 2 server instances (`ServerCapabilities/SoftwareCertificates`, the `Opc.Ua`
    binary dictionary) get typed empty values
  - 13 templates inside type definitions get typed neutral defaults

  Verified by reading every variable in the address space (0 empty values left) and
  by an ABB 800xA nodeset upload. The open62541 library itself is unchanged.

## 1.0.0 (2026-10-02)

First published version.

- OPC UA server (open62541 v1.5.8) on the M5Stack Core2, ESP-IDF v5.5
- Address space: battery, IMU (with pitch/roll tilt), microphone, touch, RTC,
  system status, writable display, side lights, Vibrate method
- Screen with Main / Motion / System pages and on-screen Light / Color / Buzz buttons
- Side LEDs on the M5GO Bottom2 base, controllable from the screen and over OPC UA
- Idle sleep: screen and lights turn off after `Display.IdleTimeout` seconds
- Battery saving: CPU frequency scaling, Wi-Fi modem sleep, microphone off by default
  (`Microphone.Enabled`), 5 V base output off while the lights are dark
- Wi-Fi: joins the strongest node on mesh networks, supports 802.11r, logs disconnect reasons
- Works with Matrikon OPC UA Explorer (requests without a timestamp are accepted)
