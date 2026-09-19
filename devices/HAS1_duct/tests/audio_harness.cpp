// Compiled with the actual audio_queue.ino, sensor phrase builders and duration table.
void completeTrack() {
    check(mp3_track_playing, "track is playing before advancing");
    now += mp3_active_phrase.tracks[mp3_active_track].duration_ms + MP3_TRACK_MARGIN_MS;
    Mp3Run();
}
void drainAudio() {
    for (int i = 0; i < 40 && mp3_track_playing; ++i) completeTrack();
    check(!mp3_track_playing && !mp3_phrase_active && mp3_pending_count == 0, "queue drains completely");
}
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    String test = argv[1]; game_state = activate; cooltime_set = 5; cooltime_add = 0;
    my["device_name"] = "duct"; mp3_available = true;
    if (test == "audio_fifo") {
        cooltime = 28; duct_available = false;
        check(Mp3TrackDurationMs(1, 2) == 3289, "measured V2 opening track fixture");
        Mp3PlayLargeFolder(1, 2); RemainingTimeMp3(1, 3, 28); Mp3PlayLargeFolder(4, 1);
        check(audioEvents == std::vector<String>{"play:1:2"}, "queued cooldown cannot interrupt opening");
        now = 3388; Mp3Run(); check(audioEvents.size() == 1, "opening duration plus margin has not elapsed");
        ++now; Mp3Run(); check(audioEvents.back() == "play:1:3", "cooldown starts after full opening duration");
        myDFPlayer.events = 9; Mp3Run(); check(audioEvents.size() == 2, "DFPlayer events do not prematurely complete track");
        drainAudio();
        check(audioEvents == std::vector<String>{"play:1:2", "play:1:3", "play:3:28", "play:1:5", "play:4:1"},
              "whole phrase plays in order without interleaved request");
        const int folders[] = {1, 1, 3, 1}; const int files[] = {2, 3, 28, 5};
        for (int i = 0; i < 4; ++i)
            check(audioStartTimes[i + 1] - audioStartTimes[i] >= Mp3TrackDurationMs(folders[i], files[i]) + MP3_TRACK_MARGIN_MS,
                  "every track completes before the next starts");
    } else if (test == "audio_door_timers") {
        openNormal(); CooltimeMp3();
        check(now == 0 && relay == HIGH, "queue registration is nonblocking");
        advance(3388); check(relay == HIGH && audioEvents.size() == 1, "opening completes before the next track");
        advance(1); check(relay == HIGH && audioEvents.back() == "play:1:3", "queued announcement begins at 3389ms without closing door early");
        advance(610); check(relay == HIGH, "door remains open until its close deadline");
        advance(1); check(relay == LOW && current_time == 0 && audioEvents.size() == 2,
                          "door closes at 4000ms while cooldown audio is playing");
        advance(2000); check(current_time == 2 && !duct_available, "cooldown timer progresses while queued speech plays");
        finished();
        for (const auto& event : audioEvents) check(event.rfind("delay:", 0) != 0, "no blocking delays in audio path");
    } else if (test == "audio_v2_blockade") {
        check(Mp3TrackDurationMs(4, 1) == 4548 && Mp3TrackDurationMs(4, 2) == 2308,
              "measured V2 blockade track fixtures");
        Mp3PlayLargeFolder(4, 1); EnterTaggerMode(); TaggerRemainingMp3();
        now = 4647; Mp3Run(); check(audioEvents == std::vector<String>{"play:4:1"},
                                  "longer V2 blockade confirmation cannot be cut off");
        ++now; Mp3Run(); check(audioEvents.back() == "play:4:2", "blockade intro begins after full confirmation");
        now += 2407; Mp3Run(); check(audioEvents.size() == 2, "V2 blockade intro retains its full duration and margin");
        ++now; Mp3Run(); check(audioEvents.back() == "play:3:26", "number begins exactly after V2 blockade intro");
        drainAudio();
        check(audioEvents == std::vector<String>{"play:4:1", "play:4:2", "play:3:26", "play:1:5"},
              "V2 blockade announcement finishes in order");
    } else if (test == "audio_overflow") {
        Mp3PlayLargeFolder(1, 2);
        Mp3PlayLargeFolder(1, 1); Mp3PlayLargeFolder(1, 4); Mp3PlayLargeFolder(1, 5); Mp3PlayLargeFolder(4, 1);
        check(mp3_pending_count == MP3_QUEUE_CAPACITY, "pending queue reaches capacity");
        RemainingTimeMp3(4, 2, 28);
        check(mp3_pending_count == MP3_QUEUE_CAPACITY, "overflow rejects whole phrase");
        drainAudio();
        check(audioEvents == std::vector<String>{"play:1:2", "play:1:1", "play:1:4", "play:1:5", "play:4:1"},
              "overflow does not play partial phrase or overwrite FIFO entries");
    } else if (test == "audio_duplicate") {
        cooltime = 30; current_time = 19; duct_available = false;
        Mp3PlayLargeFolder(1, 2);
        for (int i = 0; i < 200; ++i) RemainingTimeMp3(1, 3, 20 - (i % 10));
        check(mp3_pending_count == 1, "repeated pending announcement remains bounded");
        check(mp3_active_phrase.tracks[0].file == 2 && mp3_active_phrase.count == 1, "active opening is never rewritten");
        check(mp3_pending[0].tracks[1].file == 11, "pending phrase uses latest remaining time");
        completeTrack();
        for (int i = 0; i < 200; ++i) RemainingTimeMp3(1, 3, 7);
        check(mp3_pending_count == 0 && mp3_active_phrase.tracks[1].file == 11,
              "requests matching active phrase neither restart it nor add replay backlog");
        drainAudio();
        check(audioEvents == std::vector<String>{"play:1:2", "play:1:3", "play:3:11", "play:1:5"},
              "duplicate replacement preserves complete active and pending phrases");
    } else if (test == "audio_missing") {
        mp3_available = false; Mp3PlayLargeFolder(1, 2); RemainingTimeMp3(1, 3, 28); Mp3Run();
        check(audioEvents.empty() && mp3_pending_count == 0 && !mp3_phrase_active, "missing hardware safely discards requests");
        mp3_available = true; Mp3PlayLargeFolder(1, 2);
        check(Mp3TrackDurationMs(3, 0) == 0, "zero-second file is absent in measured fixture");
        RemainingTimeMp3(1, 3, 0);
        check(mp3_pending_count == 0, "missing number rejects entire phrase before intro queues");
        mp3_available = false; Mp3Run();
        check(!mp3_track_playing && !mp3_phrase_active && mp3_pending_count == 0, "hardware loss clears scheduler safely");
    } else if (test == "audio_wrap") {
        // Host unsigned-long rollover exercises the same unsigned elapsed arithmetic as ESP32.
        now = std::numeric_limits<unsigned long>::max() - 1000;
        Mp3PlayLargeFolder(1, 2); Mp3PlayLargeFolder(1, 1);
        now += 3388; Mp3Run(); check(audioEvents.size() == 1, "millis rollover cannot prematurely finish track");
        ++now; Mp3Run(); check(audioEvents.size() == 2 && audioEvents.back() == "play:1:1", "millis rollover advances at duration boundary");
    } else if (test == "audio_language") {
        cooltime = 28; duct_available = false;
        shift_machine["selected_language"] = "EN"; Mp3PlayLargeFolder(1, 2);
        RemainingTimeMp3(1, 3, 28);
        shift_machine["selected_language"] = "KR";
        drainAudio();
        check(audioEvents == std::vector<String>{"play:5:2", "play:5:3", "play:7:28", "play:5:5"},
              "queued language stays fixed for complete phrase");
        shift_machine["selected_language"] = "EN";
        check(Mp3TrackDurationMs(8, 2) == 0, "English blockade intro absent in fixture");
        auto count = audioEvents.size(); RemainingTimeMp3(4, 2, 28);
        check(audioEvents.size() == count && mp3_pending_count == 0, "missing English intro rejects entire phrase");
    } else if (test == "audio_stale") {
        cooltime = 28; duct_available = false;
        Mp3PlayLargeFolder(1, 2); CooltimeMp3(); current_time = 10; completeTrack();
        check(mp3_active_phrase.tracks[1].file == 18, "remaining time refreshes when queued phrase starts");
        EnterTaggerMode(); TaggerRemainingMp3(); ExitTaggerMode(); drainAudio();
        check(audioEvents == std::vector<String>{"play:1:2", "play:1:3", "play:3:18", "play:1:5"},
              "active phrase finishes while obsolete pending blockade phrase is discarded");
        audioEvents.clear(); Mp3PlayLargeFolder(1, 2); CooltimeMp3(); SettingFunc(); drainAudio();
        check(audioEvents == std::vector<String>{"play:1:2"}, "game reset discards obsolete queued countdown without interrupting active audio");
    } else if (test == "audio_folder9_language") {
        check(Mp3TrackDurationMs(9, 712) == 3289 && Mp3TrackDurationMs(9, 719) == 1153,
              "measured V2 folder 09 Korean opening fixtures");
        check(Mp3TrackDurationMs(10, 712) == 2400 && Mp3TrackDurationMs(10, 719) == 1153,
              "measured V2 folder 10 English opening fixtures");
        Mp3PlayLargeFolder(9, 719); drainAudio();
        check(audioEvents == std::vector<String>{"play:9:719"}, "Korean keeps folder 09");
        shift_machine["selected_language"] = "EN";
        Mp3PlayLargeFolder(9, 712); Mp3PlayLargeFolder(9, 719); Mp3PlayLargeFolder(1, 2); drainAudio();
        check(audioEvents == std::vector<String>{"play:9:719", "play:10:712", "play:10:719", "play:5:2"},
              "English maps folder 09 to 10 and other folders by +4");
        check(audioStartTimes[2] - audioStartTimes[1] == 2400 + MP3_TRACK_MARGIN_MS,
              "English outside opening uses its own measured length");
    } else return 2;
    std::cout << "PASS " << test << '\n';
}
