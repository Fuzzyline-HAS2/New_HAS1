// The starter overlay owns only the green background; blue progress is preserved.
static int taggerFeedbackPhase = 0; // 0 idle, 1 audio, 2..5 off/on/off/on
static unsigned long taggerBlinkStarted = 0;

void TaggerReset() {
    starterTaggerActive = false;
    taggerPurpleVisible = true;
    taggerFeedbackPhase = 0;
    taggerLastTagState = false;
    starterRfidNeedsValidation = true;
    RfidPresenceReset();
}

void TaggerEnter() {
    bool fromStarter = ptrCurrentMode == StarterActivate;
    EncoderDetach();
    TaggerReset();
    starterTaggerActive = fromStarter;
    ptrRfidMode = WaitFunc;
    ptrCurrentMode = TaggerRfidLoop;
    BlinkTimer.deleteTimer(blinkTimerId);
    if (starterTaggerActive) {
        starterContribLastCnt = StarterGaugeCnt();
        StarterGaugeUpdate(true);
    } else {
        AllNeoOn(PURPLE);
    }
}

bool TaggerFeedbackBusy() {
    return taggerFeedbackPhase != 0;
}

void TaggerFeedbackStart() {
    Mp3TaggerStart();
    taggerFeedbackPhase = 1;
}

void TaggerFeedbackLoop() {
    if (taggerFeedbackPhase == 1) {
        if (!Mp3TaggerFinished()) return;
        taggerBlinkStarted = millis();
        taggerFeedbackPhase = 2;
        taggerPurpleVisible = false;
        StarterGaugeUpdate(true);
    } else if (taggerFeedbackPhase >= 2) {
        unsigned long phase = (millis() - taggerBlinkStarted) / 400;
        int nextPhase = phase >= 4 ? 0 : 2 + phase;
        if (nextPhase != taggerFeedbackPhase) {
            taggerFeedbackPhase = nextPhase;
            taggerPurpleVisible = nextPhase == 0 || (phase % 2 == 1);
            StarterGaugeUpdate(true);
        }
    }
}
