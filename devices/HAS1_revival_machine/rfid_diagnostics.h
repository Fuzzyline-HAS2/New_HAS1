#ifndef REVIVAL_RFID_DIAGNOSTICS_H
#define REVIVAL_RFID_DIAGNOSTICS_H

// Types are in a header so Arduino's generated .ino prototypes can see them.
enum RfidDiagnosticOperation { RFID_DIAG_GAIN, RFID_DIAG_UID, RFID_DIAG_READ,
                               RFID_DIAG_RF_OFF, RFID_DIAG_RF_ON };
struct RfidDiagnosticStage {
  uint32_t started_us;
  uint32_t elapsed_us;
  int gain_db;
  uint8_t operation;
  bool ok;
  uint8_t data[10];
  uint8_t data_length;
};

extern uint16_t rfid_diag_timeout_ms;
extern uint8_t rfid_diag_retries;
extern uint32_t rfid_diag_chip_firmware;
extern bool rfid_diag_sam_ok;
extern bool rfid_diag_retries_ok;
extern bool rfid_diag_gain_ok;

void RfidDiagnosticSetup();
void RfidDiagnosticLoop();
void RfidDiagnosticRecord(int operation, int gainDb, uint32_t started,
                          bool ok, const uint8_t *data, uint8_t length);
bool RfidDiagnosticHardwareInit();
bool RfidDiagnosticSetGainDb(int gainDb);
int RfidDiagnosticSoftwareGainDb();
bool RfidDiagnosticRead(bool autoGain, uint8_t *data);
bool RfidDiagnosticSetRetries(uint8_t retries);

#endif
