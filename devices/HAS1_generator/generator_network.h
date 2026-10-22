#ifndef GENERATOR_NETWORK_H
#define GENERATOR_NETWORK_H
#include <stdint.h>
#include <has2_bounded_transport.h>
#include <utility>
enum class NetworkDiscard:uint8_t {None,Cancelled,TagChanged,GameChanged,WireChanged,WireUnstable,QueueExpired,ResultExpired};

enum class NetworkKind : uint8_t { Poll, Refresh, Tag, Wire, Completion, Verify, Contribution, Report, Watchdog };
struct NetworkRequest {
  char path[768];
  uint32_t id, generation, enqueued, started, token;
  int expectedWireCount;
  NetworkKind kind;
  bool mutation;
  bool cancelled;
  void (*callback)(const NetworkRequest &,const Has2BoundedTransport::Result &,bool);
};

// Unfenced legacy completion can modify a newer server game after a timeout.
// Deployment requires an epoch-checked server completion contract. Bench-only
// opt-in retains legacy Send semantics; it does not solve that server race.
#ifndef GENERATOR_ALLOW_UNFENCED_COMPLETION
#define GENERATOR_ALLOW_UNFENCED_COMPLETION 0
#endif
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
