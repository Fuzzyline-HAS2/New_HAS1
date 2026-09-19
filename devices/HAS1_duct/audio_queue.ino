#include "HAS1_duct.h"
#include "mp3_durations.h"

// 영어 음원 폴더: 01~04 → 05~08(+4). 폴더 09(개방 안내 합성음)는 영어판이 10이다.
int Mp3LanguageFolder(uint8_t folder, bool english)
{
    if (!english) return folder;
    return folder == 9 ? 10 : folder + 4;
}

Mp3Phrase Mp3MakePhrase(uint8_t folder, uint16_t file)
{
    Mp3Phrase phrase = {};
    bool english = (String)(const char *)shift_machine["selected_language"] == "EN";
    phrase.count = 1;
    phrase.volume = english ? 26 : 30;
    phrase.tracks[0].folder = (uint8_t)Mp3LanguageFolder(folder, english);
    phrase.tracks[0].file = file;
    return phrase;
}

bool Mp3PreparePhrase(Mp3Phrase &phrase, bool refresh_remaining)
{
    if (phrase.count == 0 || phrase.count > 3) return false;
    if (refresh_remaining && phrase.remaining_source != MP3_REMAINING_NONE)
    {
        if (game_state != activate) return false;
        if (phrase.remaining_source == MP3_REMAINING_TAGGER && !tagger_mode) return false;
        if (phrase.remaining_source == MP3_REMAINING_COOLDOWN && (duct_available || tagger_mode)) return false;
        int remaining = phrase.remaining_source == MP3_REMAINING_TAGGER
            ? TaggerRemainingSeconds() : cooltime - current_time;
        if (remaining < 0) remaining = 0;
        uint8_t language_offset = phrase.volume == 26 ? 4 : 0;
        int minutes = remaining / 60;
        phrase.tracks[1].folder = (minutes > 0 ? 2 : 3) + language_offset;
        phrase.tracks[1].file = minutes > 0 ? minutes : remaining % 60;
        phrase.tracks[2].file = minutes > 0 ? 4 : 5;
    }
    // 문장 전체를 확인한 뒤 넣어야 파일이 없을 때 안내가 중간에 끊기지 않는다.
    for (uint8_t i = 0; i < phrase.count; ++i)
    {
        Mp3Track &track = phrase.tracks[i];
        track.duration_ms = Mp3TrackDurationMs(track.folder, track.file);
        if (track.duration_ms == 0)
        {
            Serial.print("[MP3] phrase skipped: unknown track ");
            Serial.print(track.folder);
            Serial.print("/");
            Serial.println(track.file);
            return false;
        }
    }
    return true;
}

bool Mp3SamePhrase(const Mp3Phrase &left, const Mp3Phrase &right)
{
    return left.count == right.count &&
        left.tracks[0].folder == right.tracks[0].folder &&
        left.tracks[0].file == right.tracks[0].file;
}

void Mp3QueuePhrase(Mp3Phrase phrase)
{
    if (!mp3_available) return;
    if (mp3_phrase_active && Mp3SamePhrase(mp3_active_phrase, phrase)) return;
    if (!Mp3PreparePhrase(phrase, false)) return;
    // 반복 요청은 대기 중인 같은 안내의 숫자만 갱신한다. 재생 중인 문장은 유지한다.
    for (uint8_t i = 0; i < mp3_pending_count; ++i)
    {
        if (Mp3SamePhrase(mp3_pending[i], phrase))
        {
            mp3_pending[i] = phrase;
            return;
        }
    }
    if (mp3_pending_count >= MP3_QUEUE_CAPACITY)
    {
        Serial.println("[MP3] phrase skipped: queue full");
        return;
    }
    mp3_pending[mp3_pending_count++] = phrase;
    Mp3Run();
}

void Mp3Run()
{
    if (!mp3_available)
    {
        mp3_pending_count = 0;
        mp3_phrase_active = false;
        mp3_track_playing = false;
        return;
    }
    // available()은 수신 이벤트 유무이며, 새 트랙을 재생해도 된다는 뜻이 아니다.
    for (uint8_t i = 0; i < 4 && myDFPlayer.available(); ++i)
    {
        myDFPlayer.readType();
        myDFPlayer.read();
    }
    if (mp3_track_playing)
    {
        unsigned long elapsed_ms = millis() - mp3_track_started_ms;
        if (elapsed_ms < mp3_active_phrase.tracks[mp3_active_track].duration_ms + MP3_TRACK_MARGIN_MS)
            return;
        mp3_track_playing = false;
        ++mp3_active_track;
    }
    if (mp3_phrase_active && mp3_active_track >= mp3_active_phrase.count)
        mp3_phrase_active = false;
    while (!mp3_phrase_active)
    {
        if (mp3_pending_count == 0) return;
        mp3_active_phrase = mp3_pending[0];
        --mp3_pending_count;
        for (uint8_t i = 0; i < mp3_pending_count; ++i)
            mp3_pending[i] = mp3_pending[i + 1];
        // 대기 중 상태가 바뀐 안내는 버리고, 남은 시간은 실제 재생 시작 시 다시 읽는다.
        if (!Mp3PreparePhrase(mp3_active_phrase, true)) continue;
        mp3_active_track = 0;
        mp3_phrase_active = true;
    }
    if (mp3_last_volume != mp3_active_phrase.volume)
    {
        myDFPlayer.volume(mp3_active_phrase.volume);
        mp3_last_volume = mp3_active_phrase.volume;
    }
    const Mp3Track &track = mp3_active_phrase.tracks[mp3_active_track];
    myDFPlayer.playLargeFolder(track.folder, track.file);
    mp3_track_started_ms = millis();
    mp3_track_playing = true;
}
