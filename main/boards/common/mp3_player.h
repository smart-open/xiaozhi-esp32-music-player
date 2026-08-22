#ifndef MP3_PLAYER_H
#define MP3_PLAYER_H
#include "music_player.h"
#include <string>
#include <vector> //vector
#include <memory> //unique_ptr
#include <atomic>
#include <ArduinoJson.h>

typedef enum {
    PlayModeSingle = 0, //單曲播放模式
    PlayModeContinuous = 1 //連續播放模式
} PlayMode;

typedef enum {
    MP3SourceUnknow = 0, //未定義 MP3 來源
    MP3SourceHTTP = 1, //HTTP MP3
    MP3SourceFATFS = 2 //SD 卡 MP3
} MP3Source;

struct LyricLine {
    uint32_t start_ms;
    char text[128];   // 固定大小，避免動態配置出現 heap fragmentation（碎片化）
};
struct MusicInfo {
    std::string song_id;
    std::string title;
    std::string artist;
    std::string cover_id;
    std::string mp3_url;
    size_t sampling_rate;
    size_t channel_count;
    std::vector<LyricLine> lyrics;
};

class Mp3Player : public MusicPlayer {
public: //Misic override
    Mp3Player(bool support_stereo = false);
    ~Mp3Player() override;

    bool Play() override;
    bool PauseResume() override; //尚未實作
    bool Stop() override ;
    bool IsPlaying() override;

public: // HttpMp3Player 方法
    bool QueryAndPlay(const std::string& song_name, const std::string& artist_name, std::string& query_result);
    static void SetSubsonicBaseUrl(const std::string& url);
    static bool ParseStreamUrl(const std::string& url, std::string& base, std::string& song_id);

private:
    MP3Source mp3_source_ = MP3SourceUnknow;
    bool support_stereo_ = false;
    bool is_playing_ = false;
    std::atomic<bool> stop_flag_{false};
    MusicInfo current_music_info_ = {}; //宣告、初始化（initialization）時可以這樣寫。後續清空也只需要 current_music_info_ = {};

    //連續播放模式
    PlayMode play_mode_ = PlayModeSingle; //播放模式
    std::vector<MusicInfo> playlists_ = {}; //播放的曲目清單

    PlayMode get_play_mode();
    bool set_play_mode(const PlayMode mode);

    bool get_music_info(const std::string& song_name, const std::string& artist_name);
    bool get_song_lyrics(const std::string& song_id);
    std::string build_stream_url(const std::string &song_id);
    std::string build_cover_url(const std::string &cover_id);
    bool get_cover_by_coverid(const std::string& cover_id, uint8_t** out_buf, size_t* out_size);
    void show_cover_by_coverid(const std::string& cover_id);
    bool random_choose_song();

    //HTTP
    bool get_music_info_from_http(const std::string& song_name, const std::string& artist_name);
    bool get_song_lyrics_from_http(const std::string& song_id);
    bool parse_jsondoc_to_musicinfo(const JsonDocument &doc, const bool random = false);
    bool parse_jsondoc_to_lyric(const JsonDocument &doc);

    std::string build_stream_url_http(const std::string &song_id);
    std::string build_cover_url_http(const std::string &cover_id);

    bool get_cover_by_coverid_http(const std::string& cover_id, uint8_t** out_buf, size_t* out_size);

    bool get_music_info_from_fatfs(const std::string& song_name, const std::string& artist_name);
    bool get_song_lyrics_from_fatfs(const std::string& song_id);
    bool get_cover_by_coverid_fatfs(const std::string& cover_id, uint8_t** out_buf, size_t* out_size);

    void continuous_playing(); //連續播放模式入口
    static void play_scheduler_task(void* arg);
    static void streaming_task(void* arg);
    bool start_streaming_pipeline();

};
#endif

/*
HttpMp3Player player;          // ✅ OK
Music* music = &player;       // ✅ 多型
music->Play();                // ✅ 動態繫結
*/

/* _current_music_info 已初始化，要重置的話，最好不要用 {0}

正確用法是 _current_music_info = MusicInfo{};

初始化可以用 {} / {0}，
但「重新賦值」只能用「一個完整的 C++ 物件」

*/