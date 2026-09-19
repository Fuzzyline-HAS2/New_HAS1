#pragma once

#include "FreeRTOS.h"

#define portENTER_CRITICAL_SAFE(mux) testEnterCritical(mux)
#define portEXIT_CRITICAL_SAFE(mux) testExitCritical(mux)
