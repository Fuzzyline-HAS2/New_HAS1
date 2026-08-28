// [DFPlayer 비활성화] DFPlayer 원인 분리 테스트용 주석처리
// //****************************************mp3_setup()****************************************************************
// bool dfPlayerReady = false;
//
// void Mp3_Setup(){
//   MP3Serial.begin(9600);
//   Serial.println("DFRobot DFPlayer Mini Demo");
//   Serial.println("Initializing DFPlayer ... (May take 3~5 seconds)");
//   myDFPlayer.setTimeOut(1000);
//   if (!myDFPlayer.begin(MP3Serial, false)) {
//     Serial.println("Unable to begin:");
//     Serial.println("1.Please recheck the connection!");
//     Serial.println("2.Please insert the SD card!");
//     dfPlayerReady = false;
//     Serial.println("DFPlayer skipped. Continuing without audio.");
//     return;
//   }
//   dfPlayerReady = true;
//   Serial.println(F("DFPlayer Mini online."));
//   myDFPlayer.setTimeOut(500);
//   myDFPlayer.volume(30);
//   myDFPlayer.EQ(DFPLAYER_EQ_NORMAL);
//   myDFPlayer.outputDevice(DFPLAYER_DEVICE_SD);
// }
//
// void Mp3PlayLargeFolder(uint8_t folder_number, uint16_t file_number)
// {
//   if(!dfPlayerReady) return;
//   static uint8_t play_error_count = 0;
//   const char* lang = (const char *)my["selected_language"];
//   if (lang != nullptr && String(lang) == "EN")
//   {
//     folder_number = 2 + folder_number;
//   }
//   if (myDFPlayer.available())
//   {
//     myDFPlayer.playLargeFolder(folder_number, file_number);
//     play_error_count = 0;
//   }
//   else
//   {
//     if (play_error_count < 3)
//     {
//       myDFPlayer.playLargeFolder(folder_number, file_number);
//       play_error_count++;
//       Serial.print("에러횟수 :");
//       Serial.println(play_error_count);
//     }
//     else
//     {
//       if (!(send_mp3_err))
//       {
//         send_mp3_err = true;
//         has2wifi.Send((String)(const char *)my["device_name"], "device_state", "MP3");
//       }
//     }
//   }
// }

bool dfPlayerReady = false;
void Mp3_Setup() {}
void Mp3PlayLargeFolder(uint8_t folder_number, uint16_t file_number) {}