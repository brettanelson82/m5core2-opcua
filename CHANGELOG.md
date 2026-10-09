# Changelog

The firmware version is `FIRMWARE_VERSION` in `main/opcua_server.c`, readable over
OPC UA as **M5Core2 → System → FirmwareVersion**.

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
