#include "mp3_player.h"
#include "board.h"
#include "display.h"

#define TAG "Mp3Player_HTTP"

#pragma region HTTP 全域變數
// Subsonic 伺服器位址：預設為燒錄值，play_url 執行時可由播放 URL 動態推導覆蓋（免燒錄、自動適配位址變化）
static std::string base_url = CONFIG_SUBSONICAPI_URL;
const static std::string subsonic_api_para = CONFIG_SUBSONICAPI_PARA; //"u=admin&p=1111&s=raw&v=1.16.1&c=xiaozhi";

void Mp3Player::SetSubsonicBaseUrl(const std::string& url){
    if(!url.empty()){
        base_url = url;
    }
}

/**
 * @brief deserializeJson 時使用此類物件，並搭配 filter 時，可實現串流解析 JSON 並大幅縮小 Subsonic API 的 JSON 體積 
 */
class HttpStreamReader {
public:
    HttpStreamReader(Http* http)
    {
        _http = http;
        _pos = 0;
        _len = 0;
    }

    int read()
    {
        if (_pos >= _len)
        {
            _len = _http->Read(_buffer, sizeof(_buffer));
            _pos = 0;

            if (_len <= 0)
                return -1;
        }

        return _buffer[_pos++];
    }
private:
    Http* _http;

    static const int BUFFER_SIZE = 512;
    char _buffer[BUFFER_SIZE];

    int _pos;
    int _len;
};

// URL 解码（%XX 十六进制与 '+' 转空格）
static std::string url_decode(const std::string &str)
{
    std::string decoded;
    char hex[3] = {0};
    for (size_t i = 0; i < str.length(); i++)
    {
        if (str[i] == '%' && i + 2 < str.length() &&
            isxdigit((unsigned char)str[i + 1]) && isxdigit((unsigned char)str[i + 2]))
        {
            hex[0] = str[i + 1];
            hex[1] = str[i + 2];
            decoded += (char)strtol(hex, nullptr, 16);
            i += 2;
        }
        else if (str[i] == '+')
        {
            decoded += ' ';
        }
        else
        {
            decoded += str[i];
        }
    }
    return decoded;
}

bool Mp3Player::ParseStreamUrl(const std::string& url, std::string& base, std::string& song_id){
    /* 從 Subsonic stream URL 解析伺服器位址與歌曲 id
       例：http://192.168.31.116:8000/subsonic/rest/stream.view?u=admin&p=...&id=xxx.mp3
       -> base = http://192.168.31.116:8000/subsonic/rest
       -> song_id = xxx.mp3（URL 解碼後的檔名） */
    size_t qmark = url.find('?');
    if (qmark == std::string::npos || qmark == 0) {
        return false;
    }
    std::string path = url.substr(0, qmark);
    size_t slash = path.rfind('/');
    if (slash == std::string::npos) {
        return false;
    }
    std::string endpoint = path.substr(slash + 1);
    if (endpoint != "stream.view" && endpoint != "stream") {
        return false;
    }
    base = path.substr(0, slash);

    // 在查詢字串中找 id 參數
    std::string query = url.substr(qmark + 1);
    size_t pos = 0;
    while (pos < query.length()) {
        size_t amp = query.find('&', pos);
        std::string kv = query.substr(pos, (amp == std::string::npos ? query.length() : amp) - pos);
        size_t eq = kv.find('=');
        if (eq != std::string::npos && kv.substr(0, eq) == "id") {
            song_id = url_decode(kv.substr(eq + 1));
            return !song_id.empty();
        }
        if (amp == std::string::npos) break;
        pos = amp + 1;
    }
    return false;
}

// URL编码函数
static std::string url_encode(const std::string &str)
{
    std::string encoded;
    char hex[4];

    for (size_t i = 0; i < str.length(); i++)
    {
        unsigned char c = str[i];

        if ((c >= 'A' && c <= 'Z') ||
            (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~')
        {
            encoded += c;
        }
        else if (c == ' ')
        {
            encoded += '+'; // 空格编码为'+'或'%20'
        }
        else
        {
            snprintf(hex, sizeof(hex), "%%%02X", c);
            encoded += hex;
        }
    }
    return encoded;
}

/**
 * @brief 根據 song_id 生成 Subsonic API 播放連結 。
 * 
 * @param song_id       歌曲 id
 *
 * @return std::string  播放連結
 */
std::string Mp3Player::build_stream_url_http(const std::string &song_id){
    return base_url + "/stream.view?" + subsonic_api_para + "&id=" + url_encode(song_id);
}

std::string Mp3Player::build_cover_url_http(const std::string &cover_id){
    return base_url + "/getCoverArt.view?" + subsonic_api_para + "&size=220&id=" + url_encode(cover_id);
}


/**
 * 
 * @brief 將 Subsonic API 回應 JSON 轉為 JsonDocument 格式，並透過 deserializeJson 實現串流解析 JSON 。
 * @note 使用 ArduinoJson 與自訂 HttpStreamReader
 * 
 * @param full_url      要請求的完整 URL
 * @param response      回傳的原始內容
 *
 * @return true         請求成功
 * @return false        請求失敗
 */
bool get_subsonic_response(std::string& full_url, JsonDocument &response)
{
    auto network = Board::GetInstance().GetNetwork();
    auto http = network->CreateHttp(0);

    // 低功耗 WiFi 下接收延迟可达数秒，1.5s 会误判超时
    http->SetTimeout(10000);

    // 開啟 HTTP
    if (!http->Open("GET", full_url))
    {
        ESP_LOGE(TAG, "HTTP open failed");
        return false;
    }

    int status = http->GetStatusCode();

    if (status != 200)
    {
        ESP_LOGE(TAG, "HTTP status %d", status);
        http->Close();
        return false;
    }

    ESP_LOGI(TAG, "Start streaming JSON...");
    // JSON filter（只取需要欄位）
    JsonDocument filter;

    //三種查詢過濾器寫在一起即可
    //歌曲查詢
    filter["subsonic-response"]["searchResult2"]["song"][0]["id"] = true;
    filter["subsonic-response"]["searchResult2"]["song"][0]["title"] = true;
    filter["subsonic-response"]["searchResult2"]["song"][0]["artist"] = true;
    filter["subsonic-response"]["searchResult2"]["song"][0]["coverArt"] = true;
    filter["subsonic-response"]["searchResult2"]["song"][0]["channelCount"] = true;
    filter["subsonic-response"]["searchResult2"]["song"][0]["samplingRate"] = true;
    //隨機歌曲
    filter["subsonic-response"]["randomSongs"]["song"][0]["id"] = true;
    filter["subsonic-response"]["randomSongs"]["song"][0]["title"] = true;
    filter["subsonic-response"]["randomSongs"]["song"][0]["artist"] = true;
    filter["subsonic-response"]["randomSongs"]["song"][0]["coverArt"] = true;
    filter["subsonic-response"]["randomSongs"]["song"][0]["channelCount"] = true;
    filter["subsonic-response"]["randomSongs"]["song"][0]["samplingRate"] = true;

    //歌詞過濾器
    filter["subsonic-response"]["lyricsList"]["structuredLyrics"][0]["line"][0]["start"] = true;
    filter["subsonic-response"]["lyricsList"]["structuredLyrics"][0]["line"][0]["value"] = true;
    // 建立 reader
    HttpStreamReader http_reader(http.get());

    // 直接從 HTTP stream 解析 JSON
    DeserializationError err = deserializeJson(response, http_reader, DeserializationOption::Filter(filter));
    http->Close(); //<-- 這裡關掉是安全的

    if (err)
    {
        ESP_LOGE(TAG, "JSON parse error: %s", err.c_str());
        return false;
    }
    /* TEST: 取得 song array
    JsonArray arr = response["subsonic-response"]["randomSongs"]["song"].as<JsonArray>();

    if (!arr || arr.size() == 0) {
        ESP_LOGE(TAG, "JSON parse error: %s", err.c_str());
    } */
    return true;
}

#pragma endregion
/**
 * @brief 透過查詢條件（歌曲/歌手），建立 current_music_info_ 與 playlists_
 *
 * @param song_name      歌曲名稱
 * @param artist_name    演唱者/藝術家
 *
 * @return true         請求成功
 * @return false        請求失敗
 */
bool Mp3Player::get_music_info_from_http(const std::string& song_name, const std::string& artist_name){
    std::string full_query_song_url;
    bool random = false;
    if(song_name == "" && artist_name == ""){
        random = true;
        full_query_song_url = base_url + "/getRandomSongs.view?" + subsonic_api_para + "&f=json&size=100";
    }
    else{
        full_query_song_url = base_url + "/search2.view?" + subsonic_api_para + "&f=json&artistCount=0&albumCount=0&songCount=100&query=" + url_encode(song_name) + url_encode(" ") + url_encode(artist_name);
    }
    ESP_LOGI(TAG, "查詢位址 URL: %s", full_query_song_url.c_str());

    JsonDocument doc;
    if(!get_subsonic_response(full_query_song_url, doc)){
        ESP_LOGE(TAG, "取得歌曲回應失敗！");
        return false;
    }
    if (!parse_jsondoc_to_musicinfo(doc,random)){
        ESP_LOGE(TAG, "解析歌曲回應失敗！");
        return false;
    }
    return true;
}

/**
 * @brief 查詢結果 JsonDocument 文件，建立 current_music_info_ 與 playlists_
 *
 * @param doc       查詢結果的 JsonDocument 文件，用來生成 current_music_info_ 與 playlists_
 * @param random    get_music_info 的歌曲/歌手都空白時為 true，有條件時為 false，程序會篩選 doc 不同節點
 *
 * @return true         請求成功
 * @return false        請求失敗
 */
bool Mp3Player::parse_jsondoc_to_musicinfo(const JsonDocument &doc, const bool random){
    // 取得 song array
    JsonArrayConst songs ;
    if(random){
        songs = doc["subsonic-response"]["randomSongs"]["song"].as<JsonArrayConst>();
    }
    else{
        songs = doc["subsonic-response"]["searchResult2"]["song"].as<JsonArrayConst>();
    }
    if(!songs || songs.size() == 0){
        ESP_LOGE(TAG, "doc 沒有歌曲");
        return false;
    }

    playlists_.clear();            // 清空 playlist，size = 0, capacity 可能不變
    playlists_.shrink_to_fit(); // 請求減少容量以釋放未使用的內存

    for (JsonObjectConst song : songs)
    {
        MusicInfo m = MusicInfo{};
        m.song_id = song["id"].as<std::string>();
        m.title = song["title"].as<std::string>();
        m.artist = song["artist"].as<std::string>();
        m.cover_id = song["coverArt"].as<std::string>();
        m.sampling_rate = song["samplingRate"].as<std::size_t>();
        m.channel_count = song["channelCount"].as<std::size_t>();

        playlists_.push_back(m);

        ESP_LOGI(TAG,"歌曲：%s, 歌手：%s", m.title.c_str(), m.artist.c_str());
    }
    
    current_music_info_ = MusicInfo{}; //清空並釋放資源

    if(!random_choose_song()){
        ESP_LOGE(TAG, "random_choose_song failed");
        return false;
    }

    return true;
}

/**
 * @brief 根據 song_id 查詢並產生歌詞資料
 *
 * @param song_id       歌曲 id
 *
 * @return true         請求成功
 * @return false        請求失敗
 */
bool Mp3Player::get_song_lyrics_from_http(const std::string& song_id){    
    // 先釋放舊陣列
    current_music_info_.lyrics.clear();
    current_music_info_.lyrics.shrink_to_fit();

    std::string full_query_lyric_url = base_url + "/getLyricsBySongId.view?" + subsonic_api_para + "&f=json&id=" + url_encode(song_id);
    ESP_LOGI(TAG, "查詢歌詞位址：%s",full_query_lyric_url.c_str());

    JsonDocument doc;
    if(!get_subsonic_response(full_query_lyric_url, doc)){
        ESP_LOGE(TAG, "歌詞回應解析失敗！");
        return false;
    }
    else{
       if(!parse_jsondoc_to_lyric(doc)){
         ESP_LOGE(TAG, "解析歌詞內容失敗！");
         return false;
       }
    }
    return true;
}

/**
 * @brief 解析 JsonDocument doc 內容，寫入 current_music_info_.lyrics 歌詞容器
 * 
 * @param doc      回應的 JsonDocument 內容（從外部傳入）
 *
 * @return true         請求成功
 * @return false        請求失敗
 */
 bool Mp3Player::parse_jsondoc_to_lyric(const JsonDocument &doc){
    JsonArrayConst lyrics = doc["subsonic-response"]["lyricsList"]["structuredLyrics"][0]["line"].as<JsonArrayConst>();
    if(!lyrics || lyrics.size() == 0){
        ESP_LOGE(TAG, "doc 沒有歌詞資料！");
        return false;
    }
    // 已抓到歌詞，先釋放舊歌詞陣列
    current_music_info_.lyrics.clear();
    current_music_info_.lyrics.shrink_to_fit();

    for (JsonObjectConst line : lyrics){
        LyricLine lyric_line{};
        lyric_line.start_ms = line["start"].as<std::size_t>();
        strlcpy(lyric_line.text,
            line["value"].as<std::string>().c_str(),
            sizeof(lyric_line.text));
        current_music_info_.lyrics.push_back(lyric_line);
    }
    return !current_music_info_.lyrics.empty();
}

bool Mp3Player::get_cover_by_coverid_http(const std::string& cover_id, uint8_t** out_buf, size_t* out_size){
#ifdef CONFIG_SPIRAM
#define BUF_CAP MALLOC_CAP_SPIRAM
    const size_t MAX_COVER_SIZE = 512 * 1024;
#else
    #warning "ESP32-C6 確定陣亡了，不能播放封面！"
    return false;
#define BUF_CAP MALLOC_CAP_INTERNAL
    const size_t MAX_COVER_SIZE = 128 * 1024;
#endif
    auto network = Board::GetInstance().GetNetwork();
    auto http = network->CreateHttp(0);
    http->SetTimeout(10000);

    std::string cover_url = build_cover_url(cover_id);
    ESP_LOGI(TAG, "封面 URL: %s", cover_url.c_str());

    if (!http->Open("GET", cover_url)) {
        ESP_LOGE(TAG, "HTTP open failed");
        return false;
    }

    int status = http->GetStatusCode();
    if (status != 200) {
        ESP_LOGE(TAG, "HTTP status %d", status);
        http->Close();
        return false;
    }
    // -------- 初始化 buffer --------
    // 使用稍大 buffer 避免多次 realloc
    size_t capacity = 24 * 1024;   // 初始 24 KB
    size_t size = 0;
    uint8_t* buf = (uint8_t*)heap_caps_malloc(capacity, BUF_CAP | MALLOC_CAP_8BIT);
    if (!buf) {
        ESP_LOGE(TAG, "buf alloc failed");
        http->Close();
        return false;
    }

    // -------- 讀 HTTP chunk --------
    char tmp[1024];
    int n;
    while ((n = http->Read(tmp, sizeof(tmp))) > 0) {
        // buffer 不夠 → 擴充
        if (size + n > capacity) {
            size_t new_capacity = capacity * 2;
            if (new_capacity > MAX_COVER_SIZE) {
                ESP_LOGE(TAG, "Cover too large");
                heap_caps_free(buf);
                http->Close();
                return false;
            }
            uint8_t* new_buf = (uint8_t*)heap_caps_realloc(buf, new_capacity, BUF_CAP | MALLOC_CAP_8BIT);
            if (!new_buf) {
                ESP_LOGE(TAG, "new_buf realloc failed");
                heap_caps_free(buf);
                http->Close();
                return false;
            }
            buf = new_buf;
            capacity = new_capacity;
        }
        memcpy(buf + size, tmp, n);
        size += n;
    }

    if (n < 0) {
        ESP_LOGE(TAG, "HTTP read error");
        heap_caps_free(buf);
        http->Close();
        return false;
    }

    http->Close();

    ESP_LOGI(TAG, "Cover downloaded: %u bytes", (unsigned int)size);

    // ← 在這裡檢查 JPEG / PNG
    bool is_jpeg = (size >= 2 && buf[0] == 0xFF && buf[1] == 0xD8);
    bool is_png  = (size >= 4 && buf[0] == 0x89 && buf[1] == 0x50 &&
                            buf[2] == 0x4E && buf[3] == 0x47);

    ESP_LOGW(TAG, "Download format:%02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X .. %02X %02X",
        buf[0],buf[1],buf[2],buf[3],buf[4],buf[5],buf[6],buf[7],buf[8],buf[9],buf[10],buf[11], buf[size-2], buf[size-1]);

    if (!is_jpeg && !is_png) {
        ESP_LOGE(TAG, "Unsupported image format:%02X %02X %02X %02X",buf[0],buf[1],buf[2],buf[3]);
        heap_caps_free(buf);
        return false;
    }

    *out_buf = buf;
    *out_size = size;

    return true;
}

