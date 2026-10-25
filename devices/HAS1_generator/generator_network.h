#ifndef GENERATOR_NETWORK_H
#define GENERATOR_NETWORK_H
#include <stdint.h>
#include "generator_wire_protocol.h"
#include <has2_bounded_transport.h>
#include <utility>
enum class NetworkDiscard:uint8_t {None,Cancelled,TagChanged,GameChanged,WireChanged,WireUnstable,QueueExpired,ResultExpired};

enum class NetworkKind : uint8_t { Poll, Refresh, Tag, Wire, Completion, Verify, Contribution, Report, Watchdog };
struct NetworkRequest {
  char path[768];
  uint32_t id, generation, enqueued, started, token, wireGeneration;
  int expectedWireCount;
  NetworkKind kind;
  bool mutation;
  bool cancelled;
  bool dispatched;
  bool wireBypass;
  void (*callback)(const NetworkRequest &,const Has2BoundedTransport::Result &,bool);
};

// Generic reports retain legacy at-most-once transmission. Generator completion
// uses a durable, immutable conditional token and authoritative reconciliation
// when the server advertises protocol 1, otherwise one legacy Send.
bool GeneratorNetworkCompletionPending();
void GeneratorNetworkObserveProtocol(const GeneratorWireSnapshot &);
bool WireCompletionContext(GeneratorWireSnapshot &);
void WireRequestContextRefresh();
void GeneratorNetworkBegin();
void GeneratorNetworkLoop();
bool GeneratorNetworkRequestPoll();
bool GeneratorNetworkPollBusy();
bool GeneratorNetworkTakePollResult(bool &success);
bool GeneratorNetworkSend(const char *device,const char *column,const char *value);
bool GeneratorNetworkContribution(const char *user,int delta);
bool GeneratorNetworkTag(const char *user);
void GeneratorNetworkForgetTag();
bool GeneratorNetworkComplete(const char *state);
void GeneratorNetworkRefresh();
void GeneratorNetworkRefreshCycle();
void GeneratorNetworkRequestMaintenance(bool legacy=false);
uint32_t GeneratorNetworkGeneration();
void GeneratorNetworkInvalidateGame();
#endif
