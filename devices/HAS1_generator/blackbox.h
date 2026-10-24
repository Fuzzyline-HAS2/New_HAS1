#pragma once
#include <HAS1BlackBox.h>
#include "blackbox_policy.h"
void BlackBoxBegin();
void BlackBoxLoop();
void BlackBoxNetwork(bool busy,bool maintenance,uint32_t request,int phase,uint32_t elapsed,uint32_t progressAge,uint32_t stack);
void BlackBoxHttp(int kind,int error,int status,uint32_t elapsed,bool stale);
void BlackBoxValidSnapshot(bool valid);
void BlackBoxPollResult(bool valid);
void BlackBoxBeforeRestart();
void BlackBoxOtaRestart(uint32_t attempts);
void BlackBoxDiagnostic(uint16_t event,uint8_t phase,uint32_t id,int32_t code,uint32_t elapsed,uint32_t detail);
void NeoRetryPending();

struct NetworkRequest;
namespace Has2BoundedTransport {struct Result;}
void BlackBoxHttpDetail(const NetworkRequest &,const Has2BoundedTransport::Result &,bool stale,int discard);
void BlackBoxRecoveryState(unsigned depth,bool mailbox,bool scanPending,bool driverFault,bool maintenance);

void BlackBoxOfflineRecovery(int stage,uint32_t outageMs,uint32_t attempts,uint32_t detail);
