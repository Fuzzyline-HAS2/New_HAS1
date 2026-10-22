# Bounded HTTP parser provenance

Response parsing methods are derived from Arduino-ESP32 3.3.11 `libraries/HTTPClient/src/HTTPClient.cpp` (LGPL-2.1-or-later, original notices retained).

Original SHA-256: `bf883d05792f3dfcbd2f623e4bc5c9897fe50d16935fb6d63db4215e7215f53d`.

Only the checked opt-in path uses this scoped subclass. It reuses SDK URL/request handling and response parser structure, adding bounded CRLF lines, strict lengths/chunks, trailer completion, read deadlines and bounded body writes. The default SDK library is never modified. The socket adapter accepts numeric IPv4 HTTP only, matching the fixed local backend; DNS/TLS/redirects are rejected rather than claimed to be bounded.
