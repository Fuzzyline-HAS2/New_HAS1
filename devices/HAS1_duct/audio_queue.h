#ifndef HAS1_DUCT_AUDIO_QUEUE_H
#define HAS1_DUCT_AUDIO_QUEUE_H

struct Mp3Track {
    uint8_t folder;
    uint16_t file;
    unsigned long duration_ms;
};

enum Mp3RemainingSource { MP3_REMAINING_NONE, MP3_REMAINING_COOLDOWN, MP3_REMAINING_TAGGER };

struct Mp3Phrase {
    Mp3Track tracks[3];
    uint8_t count;
    uint8_t volume;
    Mp3RemainingSource remaining_source;
};

const uint8_t MP3_QUEUE_CAPACITY = 4;
const unsigned long MP3_TRACK_MARGIN_MS = 100;
Mp3Phrase mp3_pending[MP3_QUEUE_CAPACITY];
Mp3Phrase mp3_active_phrase;
uint8_t mp3_pending_count = 0;
uint8_t mp3_active_track = 0;
uint8_t mp3_last_volume = 0;
bool mp3_phrase_active = false;
bool mp3_track_playing = false;
unsigned long mp3_track_started_ms = 0;

void Mp3Run();
void Mp3QueuePhrase(Mp3Phrase phrase);
bool Mp3PreparePhrase(Mp3Phrase &phrase, bool refresh_remaining);
bool Mp3SamePhrase(const Mp3Phrase &left, const Mp3Phrase &right);
int Mp3LanguageFolder(uint8_t folder, bool english);
Mp3Phrase Mp3MakePhrase(uint8_t folder, uint16_t file);

#endif
