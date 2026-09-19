#pragma once
#include <stdint.h>

// Source: https://github.com/Fuzzyline-HAS2/HAS2-Nextion
// Branch: feat/audio-library-v2
// Commit: e49fa96d26ceb0c5fdc3c379c7e5a619c944ce32
// Path: audios/V2/duct_MP3_22k/01~08 (154 WAV files)
//       + 09/0712, 09/0719 (한국어 개방 안내), 10/0712, 10/0719 (영어 개방 안내).
// Python wave: ceil(nframes * 1000 / framerate), using integer arithmetic.
// 재생 여유(MP3_TRACK_MARGIN_MS)는 시퀀서에서 더한다. 음원 교체 시 길이표도 다시 측정한다.
inline unsigned long Mp3TrackDurationMs(uint8_t folder, uint16_t track)
{
    switch (folder) {
    case 1:
        switch (track) {
        case 1: return 1360UL;
        case 2: return 3289UL;
        case 3: return 2019UL;
        case 4: return 681UL;
        case 5: return 1187UL;
        default: return 0;
        }
    case 2:
        switch (track) {
        case 1: return 420UL;
        case 2: return 365UL;
        case 3: return 518UL;
        case 4: return 509UL;
        case 5: return 428UL;
        case 6: return 321UL;
        case 7: return 517UL;
        case 8: return 517UL;
        case 9: return 378UL;
        case 10: return 492UL;
        default: return 0;
        }
    case 3:
        switch (track) {
        case 1: return 420UL;
        case 2: return 365UL;
        case 3: return 518UL;
        case 4: return 509UL;
        case 5: return 428UL;
        case 6: return 321UL;
        case 7: return 517UL;
        case 8: return 517UL;
        case 9: return 378UL;
        case 10: return 492UL;
        case 11: return 677UL;
        case 12: return 698UL;
        case 13: return 849UL;
        case 14: return 733UL;
        case 15: return 612UL;
        case 16: return 727UL;
        case 17: return 823UL;
        case 18: return 786UL;
        case 19: return 695UL;
        case 20: return 681UL;
        case 21: return 798UL;
        case 22: return 797UL;
        case 23: return 814UL;
        case 24: return 889UL;
        case 25: return 856UL;
        case 26: return 884UL;
        case 27: return 1008UL;
        case 28: return 982UL;
        case 29: return 857UL;
        case 30: return 817UL;
        case 31: return 1004UL;
        case 32: return 1123UL;
        case 33: return 1046UL;
        case 34: return 999UL;
        case 35: return 1010UL;
        case 36: return 984UL;
        case 37: return 1114UL;
        case 38: return 1016UL;
        case 39: return 1087UL;
        case 40: return 903UL;
        case 41: return 944UL;
        case 42: return 909UL;
        case 43: return 1036UL;
        case 44: return 1095UL;
        case 45: return 898UL;
        case 46: return 958UL;
        case 47: return 1185UL;
        case 48: return 1011UL;
        case 49: return 1092UL;
        case 50: return 846UL;
        case 51: return 875UL;
        case 52: return 869UL;
        case 53: return 1007UL;
        case 54: return 1041UL;
        case 55: return 954UL;
        case 56: return 905UL;
        case 57: return 1054UL;
        case 58: return 994UL;
        case 59: return 1036UL;
        case 60: return 798UL;
        default: return 0;
        }
    case 4:
        switch (track) {
        case 1: return 4548UL;
        case 2: return 2308UL;
        case 3: return 4715UL;
        default: return 0;
        }
    case 5:
        switch (track) {
        case 1: return 4094UL;
        case 2: return 5066UL;
        case 3: return 1979UL;
        case 4: return 739UL;
        case 5: return 3057UL;
        default: return 0;
        }
    case 6:
        switch (track) {
        case 1: return 681UL;
        case 2: return 797UL;
        case 3: return 823UL;
        case 4: return 786UL;
        case 5: return 828UL;
        case 6: return 870UL;
        case 7: return 797UL;
        case 8: return 702UL;
        case 9: return 870UL;
        case 10: return 734UL;
        default: return 0;
        }
    case 7:
        switch (track) {
        case 1: return 681UL;
        case 2: return 797UL;
        case 3: return 823UL;
        case 4: return 786UL;
        case 5: return 828UL;
        case 6: return 870UL;
        case 7: return 797UL;
        case 8: return 702UL;
        case 9: return 870UL;
        case 10: return 734UL;
        case 11: return 828UL;
        case 12: return 845UL;
        case 13: return 867UL;
        case 14: return 841UL;
        case 15: return 841UL;
        case 16: return 960UL;
        case 17: return 978UL;
        case 18: return 915UL;
        case 19: return 915UL;
        case 20: return 804UL;
        case 21: return 1049UL;
        case 22: return 1057UL;
        case 23: return 1013UL;
        case 24: return 1044UL;
        case 25: return 1110UL;
        case 26: return 1145UL;
        case 27: return 1013UL;
        case 28: return 961UL;
        case 29: return 1044UL;
        case 30: return 727UL;
        case 31: return 978UL;
        case 32: return 1000UL;
        case 33: return 1062UL;
        case 34: return 1066UL;
        case 35: return 1027UL;
        case 36: return 1071UL;
        case 37: return 1097UL;
        case 38: return 1040UL;
        case 39: return 1005UL;
        case 40: return 727UL;
        case 41: return 947UL;
        case 42: return 1066UL;
        case 43: return 969UL;
        case 44: return 978UL;
        case 45: return 1044UL;
        case 46: return 1027UL;
        case 47: return 1035UL;
        case 48: return 872UL;
        case 49: return 1075UL;
        case 50: return 793UL;
        case 51: return 872UL;
        case 52: return 961UL;
        case 53: return 969UL;
        case 54: return 880UL;
        case 55: return 996UL;
        case 56: return 1074UL;
        case 57: return 975UL;
        case 58: return 859UL;
        case 59: return 1048UL;
        case 60: return 943UL;
        default: return 0;
        }
    case 8:
        switch (track) {
        case 3: return 2627UL;
        default: return 0;
        }
    case 9:
        switch (track) {
        case 712: return 3289UL;
        case 719: return 1153UL;
        default: return 0;
        }
    case 10:
        switch (track) {
        case 712: return 2400UL;
        case 719: return 1153UL;
        default: return 0;
        }
    default: return 0;
    }
}
