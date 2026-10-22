# Bounded plain HTTP transport

`has2_bounded_transport.h/.cpp` implements a single-owner HTTP GET exchange for a numeric IPv4 host. It does not use `HTTPClient`, DNS, TLS, redirects, connection reuse, or request retries.

`get(host, port, encodedPath, options)` returns a response whose `ok()` means transport and framing completed. HTTP status remains separate: a complete 409/500 response is `ok() == true`. Failed exchanges expose no partial payload.

The request start clock is shared across connect, write, headers, body, and cleanup. Progress never restarts that clock. `bodyDeadlineMs` is an additional body-only cap. `requireContentLength` preserves the wire CAS contract: Content-Length must be present and strictly positive, so EOF/chunked/empty responses are rejected for that mode. Unsigned elapsed arithmetic supports a millis wrap; configured deadlines must be positive and no greater than `INT32_MAX`. The default overall 2000 ms is a trial value, not a field-validated operating value.

Bounds:

- Encoded path: 2048 bytes; no spaces, control/non-ASCII bytes, or raw `#`.
- Response headers: 4096 bytes total, 1024 bytes per line, 64 field lines.
- Response body: configurable up to 16384 decoded bytes, 20480 consumed wire bytes including chunk framing/trailers.
- Interim 1xx and protocol upgrades are rejected. Identity with Content-Length, chunked, and close-delimited bodies are supported. Conflicting framing, duplicate Content-Length/Transfer-Encoding, invalid lengths, and malformed chunk framing are rejected.
- Parser storage is fixed except the bounded response String. The main request buffer is 2304 bytes; parser input buffer is 512 bytes. The worker stack must budget the request buffer and nested header parser (1025 bytes), runtime/framework overhead, and caller frames.

`phaseHook(Phase)` runs on the owning worker once at each actual phase entry; it may only publish bounded POD state (no JSON/game/RTC or blocking operations). Done marks exchange completion even on failure, while Result.phase retains the failure stage. `progressHook` runs during nonblocking waits. The owning worker may use it to service its watchdog; the main task must not feed that worker's watchdog. `connectMs/writeMs/headerMs/bodyMs/closeMs` are phase durations, `firstByteMs` is offset from request start, and `totalMs` includes cleanup. Scheduler pauses and OS/lwIP internals are not hard real-time guarantees; cleanup overrun is reported as a deadline failure rather than success.

Run:

```sh
python3 libraries/HAS2_Wifi/tests/run_bounded_transport_tests.py
SANITIZE=1 python3 libraries/HAS2_Wifi/tests/run_bounded_transport_tests.py
```

Tests compile the production parser and socket adapter. Deterministic fake-I/O cases cover no SYN completion, transmit stalls/partial progress, slow headers, missing line terminators, body deadlines vs whole-request deadline, partial/EOF responses, length and chunk framing, size caps, rollover, and cleanup overruns. A loopback TCP exchange exercises the real socket adapter. The loopback bind requires permission outside a network-restricted sandbox. These host tests do not replace ESP32 scheduling, Wi-Fi, server failure-injection, or field soak validation.
