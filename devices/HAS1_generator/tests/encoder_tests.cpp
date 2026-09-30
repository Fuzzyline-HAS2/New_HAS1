// Host-only Arduino/FreeRTOS fakes exercise production encoder.ino below.
// Does not simulate real ISR timing, electrical noise or thread scheduling.
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#define ARDUINO_ISR_ATTR
using portMUX_TYPE = int;
#define portMUX_INITIALIZER_UNLOCKED 0
static int lockDepth=0;
#define portENTER_CRITICAL(m) ((void)(m), ++lockDepth)
#define portEXIT_CRITICAL(m) (--lockDepth)
#define portENTER_CRITICAL_ISR(m) ((void)(m), ++lockDepth)
#define portEXIT_CRITICAL_ISR(m) (--lockDepth)
const int INPUT_PULLUP=1, CHANGE=2, encoderPinA=13, encoderPinB=15;
bool encoderAttached=false;
long encoderValue=0;
int gameTimerCnt=0;
static int pins=0, registrations=0;
static void (*callbacks[16])()={};
int digitalRead(int p) { return p==encoderPinA ? (pins>>1)&1 : pins&1; }
int digitalPinToInterrupt(int p) { return p; }
void pinMode(int,int) {}
void attachInterrupt(int p,void(*fn)(),int mode) { assert(!lockDepth && mode==CHANGE); callbacks[p]=fn; ++registrations; }
void detachInterrupt(int p) { assert(!lockDepth); callbacks[p]=nullptr; --registrations; }
void EncoderLoop();
#include "../encoder.ino"
static void event(int state) { pins=state; EncoderInterrupt(); assert(lockDepth==0); }
static void start(int state) { EncoderDetach(); pins=state; encoderValue=0; gameTimerCnt=9; EncoderAttach(); assert(registrations==2); }
int main() {
 EncoderInit(); assert(!encoderAttached && registrations==0); event(1); EncoderLoop(); assert(encoderValue==0);
 // Independent truth: all and only transitions with Hamming distance one count.
 for(int before=0;before<4;++before) for(int after=0;after<4;++after) {
  start(before); event(after); assert(encoderValue==0 && gameTimerCnt==9);
  EncoderLoop(); int d=before^after; assert(encoderValue==(d==1||d==2));
  assert(gameTimerCnt==((d==1||d==2)?0:9));
 }
 start(0); for(int s:{1,3,2,0}) event(s); EncoderLoop(); assert(encoderValue==4);
 start(0); for(int s:{2,3,1,0}) event(s); EncoderLoop(); assert(encoderValue==4);
 start(0); for(int s:{1,0,1,0}) event(s); EncoderLoop(); assert(encoderValue==4); // reverse/bounce preserved
 start(0); event(3); event(2); EncoderLoop(); assert(encoderValue==1); // illegal transition resynchronizes
 start(0); event(0); EncoderLoop(); assert(encoderValue==0);
 start(3); event(2); EncoderAttach(); assert(registrations==2); event(0); EncoderDetach();
 assert(encoderValue==2 && registrations==0 && !encoderAttached);
 EncoderDetach(); EncoderLoop(); assert(encoderValue==2 && registrations==0);
 event(1); EncoderLoop(); assert(encoderValue==2); // stale interrupt while disabled
 pins=3; EncoderAttach(); event(1); EncoderLoop(); assert(encoderValue==3); // seed at restart
 for(int i=0;i<100;++i) { EncoderAttach(); event(0); EncoderDetach(); EncoderDetach(); pins=3; }
 assert(registrations==0);
 start(0); for(int i=0;i<20000;++i) {event(1);event(3);event(2);event(0);} EncoderDetach(); assert(encoderValue==80000);
 start(0); event(1); EncoderDetach(); encoderValue=100; EncoderLoop(); assert(encoderValue==100);
 std::puts("PASS: exhaustive 16 transitions, direction/reversal, invalid resync, lifecycle, pending/timer handoff, >16-bit accumulation, reset ordering");
}
