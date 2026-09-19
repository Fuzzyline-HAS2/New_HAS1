#pragma once

struct portMUX_TYPE { unsigned marker; };
#define portMUX_INITIALIZER_UNLOCKED {0}

void testEnterCritical(portMUX_TYPE* mux);
void testExitCritical(portMUX_TYPE* mux);
