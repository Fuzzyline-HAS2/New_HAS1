#include "../blackbox_policy.h"
#include <assert.h>
#include <stdint.h>
using namespace GeneratorBlackBox;
int main() {
  BlackBoxSummary s;
  assert(!s.sample(UINT32_MAX-100)); assert(!s.sample(100));
  assert(s.maxGap==201 && s.loops==2);
  assert(s.sample(59900)); assert(s.maxGap==59800);
  s.emitted(59900); assert(s.maxGap==0 && s.loops==0);
  assert(!s.sample(59901)); assert(!s.sample(119899)); assert(s.sample(119900));
  BlackBoxRateGate g;
  assert(g.allow(UINT32_MAX-100)); assert(!g.allow(10)); assert(g.allow(59900));
  assert(blackBoxUploadAllowed(true,false,false,true,false,false));
  for(unsigned bit=0;bit<6;++bit) {
    bool known=true,active=false,photo=false,online=true,busy=false,ota=false;
    switch(bit){case 0:known=false;break;case 1:active=true;break;case 2:photo=true;break;
      case 3:online=false;break;case 4:busy=true;break;case 5:ota=true;break;}
    assert(!blackBoxUploadAllowed(known,active,photo,online,busy,ota));
  }
}
