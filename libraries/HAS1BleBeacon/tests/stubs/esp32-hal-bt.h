#pragma once

#include "esp_bt.h"

bool btStartMode(esp_bt_mode_t mode);
bool btStop();
bool btMemRelease(esp_bt_mode_t mode);
