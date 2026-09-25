#pragma once
#include "rfid_types.inc"
RfidReadOutcome rfid_last_outcome = RfidReadOutcome::Unavailable;
bool rfid_recovery_required = false, rfid_gain_known = true, rfid_recovery_locked = false;
uint8_t rfid_recovery_attempts = 0;
uint32_t rfid_next_recovery_ms = 0;
bool gameplay_tag_missing = false;
unsigned gameplay_tag_miss_count = 0;
uint32_t gameplay_tag_missing_since_ms = 0;
bool revival_approval_pending = false;
