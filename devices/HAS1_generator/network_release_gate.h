#pragma once

// Development build only. The currently inspected server has no generation-
// checked completion/contribution API, and duplicate repaired writes decrement
// its count again. Remove this release gate only with the implemented server
// contract and passing cross-game/duplicate-request integration evidence.
// The bench-only legacy flag must never bypass the production gate.
#ifdef GENERATOR_PRODUCTION_BUILD
#error "Generator network isolation is not deployable: server generation/idempotency contract and hardware validation are pending. See NETWORK_ISOLATION.md."
#endif
