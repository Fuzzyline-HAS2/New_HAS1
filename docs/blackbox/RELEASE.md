# Blackbox firmware release settings

The existing **Deploy Firmware** workflow retains the normal SecureOTA signing,
version increment, fixed release tag and partitions. Select `iotglove` (normal
TTGO glove) or `HAS1_generator`. Training gloves remain a separate USB build;
Beetle and other devices do not gain a blackbox uploader from this workflow.

`blackbox_mode` is an explicit choice:

- `flash` (default): flash journal only, empty MQTT URI/password. No logs reach
  Windows automatically. Existing non-blackbox targets are unchanged.
- `mqtt`: flash journal plus the bounded background MQTT uploader. Requires the
  repository Actions **variable** `BLACKBOX_BROKER_URI` and Actions **secret**
  `BLACKBOX_MQTT_PASSWORD`. Missing/invalid settings fail before version increment
  or publishing. MQTT on unsupported devices fails rather than silently ignoring it.

For store 1 the intended address is `mqtt://172.30.1.43:1883`; confirm that its
broker, collector, firewall and device accounts are ready before choosing MQTT.
The helper accepts only explicit private LAN IPv4 addresses and ports, with no
DNS, URL credentials or TLS scheme. This matches the existing trusted-LAN uploader.
Firmware uses its uppercase 12-hex-digit Wi-Fi station MAC as MQTT username.
The MQTT client ID is `has1-bb-<MAC>`. The broker must have each device's MAC account and the configured
password, with the supplied per-user topic ACL. The collector uses its separate
account. Changing only the firmware setting does not provision broker accounts.

Passwords must be 1–127 printable ASCII bytes and cannot be compile/test
placeholders. This deployment method provides the selected password to all
firmware instances built from that artifact; the header is ignored by Git and
excluded from source provenance. Like the pre-existing HMAC, the credential is
compiled into the firmware binary, so do not treat binary access as secret-free.

The generated `blackbox-release-config.json` Actions artifact records only the
mode, device, broker address and authentication type. It contains no password,
HMAC, username override or credential hash. The signing key still comes from the
unchanged `HMAC_SECRET` secret and validation. No credentials appear in command
arguments or public build configuration. The normal release assets and OTA
structure remain unchanged.

Every deploy also keeps `firmware-elf-<device>-v<version>-<run_id>` (the sketch
`.ino.elf` and `.map`, 90 days) and prints the ELF SHA256 in the log, so device
coredump PCs logged as `prior.core*` records can be decoded with addr2line. The
ELF holds the same compiled-in secrets as `update.bin`; it is deliberately not a
public Release asset. Download it only to decode a crash.

After deployment, verify one idle device produces a stored SQLite batch and ACK
before rolling out broadly. Games/photo/OTA intentionally suppress upload;
absence of a live upload during those periods is not an offline diagnosis.
