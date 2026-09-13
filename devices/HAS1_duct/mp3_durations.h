#pragma once
#include <stdint.h>

// 덕트_MP3 원본을 afinfo로 측정한 길이(ms, 올림). 재생 여유는 시퀀서에서 더한다.
// 음원을 교체하면 해당 항목도 다시 측정한다.
inline unsigned long Mp3TrackDurationMs(uint8_t folder, uint16_t track)
{
    switch (folder) {
    case 1:
        switch (track) {
        case 1: return 1385UL;
        case 2: return 3814UL;
        case 3: return 3005UL;
        case 4: return 706UL;
        case 5: return 1202UL;
        default: return 0;
        }
    case 2:
        switch (track) {
        case 1: return 549UL;
        case 2: return 471UL;
        case 3: return 654UL;
        case 4: return 654UL;
        case 5: return 523UL;
        case 6: return 445UL;
        case 7: return 627UL;
        case 8: return 575UL;
        case 9: return 445UL;
        case 10: return 575UL;
        default: return 0;
        }
    case 3:
        switch (track) {
        case 1: return 549UL;
        case 2: return 471UL;
        case 3: return 654UL;
        case 4: return 654UL;
        case 5: return 523UL;
        case 6: return 445UL;
        case 7: return 627UL;
        case 8: return 575UL;
        case 9: return 445UL;
        case 10: return 575UL;
        case 11: return 758UL;
        case 12: return 784UL;
        case 13: return 915UL;
        case 14: return 836UL;
        case 15: return 732UL;
        case 16: return 836UL;
        case 17: return 915UL;
        case 18: return 863UL;
        case 19: return 810UL;
        case 20: return 758UL;
        case 21: return 863UL;
        case 22: return 915UL;
        case 23: return 941UL;
        case 24: return 993UL;
        case 25: return 967UL;
        case 26: return 967UL;
        case 27: return 1045UL;
        case 28: return 1098UL;
        case 29: return 915UL;
        case 30: return 915UL;
        case 31: return 1071UL;
        case 32: return 1228UL;
        case 33: return 1150UL;
        case 34: return 1124UL;
        case 35: return 1098UL;
        case 36: return 1124UL;
        case 37: return 1202UL;
        case 38: return 1072UL;
        case 39: return 1150UL;
        case 40: return 993UL;
        case 41: return 1019UL;
        case 42: return 1019UL;
        case 43: return 1124UL;
        case 44: return 1150UL;
        case 45: return 1019UL;
        case 46: return 1098UL;
        case 47: return 1280UL;
        case 48: return 1072UL;
        case 49: return 1228UL;
        case 50: return 915UL;
        case 51: return 967UL;
        case 52: return 967UL;
        case 53: return 1098UL;
        case 54: return 1176UL;
        case 55: return 1072UL;
        case 56: return 993UL;
        case 57: return 1176UL;
        case 58: return 1150UL;
        case 59: return 1124UL;
        case 60: return 889UL;
        default: return 0;
        }
    case 4:
        switch (track) {
        case 1: return 1646UL;
        case 2: return 3318UL;
        case 3: return 4285UL;
        default: return 0;
        }
    case 5:
        switch (track) {
        case 1: return 4128UL;
        case 2: return 5112UL;
        case 3: return 2016UL;
        case 4: return 784UL;
        case 5: return 3109UL;
        default: return 0;
        }
    case 6:
        switch (track) {
        case 1: return 732UL;
        case 2: return 836UL;
        case 3: return 863UL;
        case 4: return 836UL;
        case 5: return 863UL;
        case 6: return 915UL;
        case 7: return 836UL;
        case 8: return 732UL;
        case 9: return 915UL;
        case 10: return 784UL;
        default: return 0;
        }
    case 7:
        switch (track) {
        case 1: return 732UL;
        case 2: return 836UL;
        case 3: return 863UL;
        case 4: return 836UL;
        case 5: return 863UL;
        case 6: return 915UL;
        case 7: return 836UL;
        case 8: return 732UL;
        case 9: return 915UL;
        case 10: return 784UL;
        case 11: return 863UL;
        case 12: return 889UL;
        case 13: return 915UL;
        case 14: return 889UL;
        case 15: return 889UL;
        case 16: return 993UL;
        case 17: return 1019UL;
        case 18: return 967UL;
        case 19: return 967UL;
        case 20: return 836UL;
        case 21: return 1098UL;
        case 22: return 1098UL;
        case 23: return 1045UL;
        case 24: return 1072UL;
        case 25: return 1150UL;
        case 26: return 1176UL;
        case 27: return 1045UL;
        case 28: return 993UL;
        case 29: return 1072UL;
        case 30: return 758UL;
        case 31: return 1019UL;
        case 32: return 1045UL;
        case 33: return 1098UL;
        case 34: return 1098UL;
        case 35: return 1072UL;
        case 36: return 1098UL;
        case 37: return 1124UL;
        case 38: return 1072UL;
        case 39: return 1045UL;
        case 40: return 758UL;
        case 41: return 993UL;
        case 42: return 1098UL;
        case 43: return 1019UL;
        case 44: return 1019UL;
        case 45: return 1072UL;
        case 46: return 1072UL;
        case 47: return 1072UL;
        case 48: return 915UL;
        case 49: return 1124UL;
        case 50: return 836UL;
        case 51: return 915UL;
        case 52: return 993UL;
        case 53: return 1019UL;
        case 54: return 915UL;
        case 55: return 1045UL;
        case 56: return 1124UL;
        case 57: return 1019UL;
        case 58: return 889UL;
        case 59: return 1098UL;
        case 60: return 993UL;
        default: return 0;
        }
    case 8:
        switch (track) {
        case 3: return 2664UL;
        default: return 0;
        }
    default: return 0;
    }
}
