#ifndef GENERATOR_NETWORK_H
#define GENERATOR_NETWORK_H
#include <stdint.h>
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

// Existing Send/Situation server API. A timeout is ambiguous: queued work can
// be cancelled locally, but an already dispatched mutation cannot be fenced by
// the legacy server. Never automatically resend uncertain mutations.
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
