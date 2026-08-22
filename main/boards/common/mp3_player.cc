#include "mp3_player.h"
#include "board.h"
#include "display.h"
#include "system_info.h"
#include "settings.h"
#include "audio/audio_codec.h"
#include "application.h"
#include <esp_log.h>
#include "assets/lang_config.h"

#include "audio_element.h"
#include "audio_pipeline.h"
#include "audio_event_iface.h"
#include "http_stream.h"
#include "fatfs_stream.h"
#include "mp3_decoder.h"
#include "raw_stream.h"
#include <esp_crt_bundle.h> //當串流平台是 https 時，http_stream 使用 TLS

#include "mcp_server.h"

#include "led/single_led.h"

#include <esp_heap_caps.h>

#define TAG "Mp3Player"
#define SD_MOUNT_POINT "/sdcard"
#define SD_FLAG_FILE SD_MOUNT_POINT "/.xiaozhi-esp32.txt"

const int pipeline_task_prio_ = 10;

#pragma region 類別成員函數 - 基本
Mp3Player::Mp3Player(bool support_stereo){
    support_stereo_ = support_stereo;
    play_mode_ = get_play_mode();
    auto& mcp_server = McpServer::GetInstance();
    mcp_server.AddTool("self.music.play_url",
            "流式播放指定 URL 的音乐。当 AI 已通过音乐服务工具（music_play / music_search）查询到歌曲的播放地址（url 字段）后，"
            "调用本工具直接播放该地址，无需再次查询。\n"
            "参数说明:\n"
            "  `url`: 音乐的完整播放地址（来自 music_play 工具返回的 song.url 字段，必填）。\n"
            "  `title`: 歌曲名称（可选，默认为空字符串）。\n"
            "  `artist`: 歌手名称（可选，默认为空字符串）。\n"
            "返回:\n"
            "  播放状态信息，不需确认。\n"
            "范例:\n"
            "  music_play 返回 url 后：'播放该 url'（url='http://192.168.x.x:8000/subsonic/rest/stream.view?...'）\n"
            "  '随机挑几首歌来播放'（先 music_search/music_play 取得 url 再调用）\n",
        PropertyList({
                 Property("url", kPropertyTypeString),//播放地址（必需）
                 Property("title", kPropertyTypeString, ""),//歌曲名称（可选，默认为空字符串）
                 Property("artist", kPropertyTypeString, "")//歌手名称（可选，默认为空字符串）
        }),
        [this](const PropertyList& properties) -> ReturnValue {
            ESP_LOGW(TAG, "MCP 執行 play_url");
            auto url = properties["url"].value<std::string>();
            if (url.empty()) {
                return "{\"success\": false, \"message\": \"url 参数不能为空\"}";
            }
            auto title = properties["title"].value<std::string>();
            auto artist = properties["artist"].value<std::string>();
            ESP_LOGI(TAG, "播放位址 URL: %s", url.c_str());
            mp3_source_ = MP3SourceHTTP;
            current_music_info_ = MusicInfo{};
            current_music_info_.title = title.empty() ? "音樂" : title;
            current_music_info_.artist = artist;
            current_music_info_.mp3_url = url;
            // 從播放 URL 解析伺服器位址與歌曲 id（供歌詞/封面查詢，免燒錄位址）
            std::string base, song_id;
            if (ParseStreamUrl(url, base, song_id)) {
                SetSubsonicBaseUrl(base);
                current_music_info_.song_id = song_id;
                current_music_info_.cover_id = song_id;
                ESP_LOGI(TAG, "解析 URL -> base=%s, song_id=%s", base.c_str(), song_id.c_str());
            } else {
                ESP_LOGW(TAG, "非 Subsonic stream URL，略過歌詞查詢");
            }
            if (!this->Play()) {
                return "{\"success\": false, \"message\": \"建立播放任務失敗\"}";
            }
            return "{\"success\": true, \"message\": \"開始播放: " + current_music_info_.title + "\"}";
    });
    ESP_LOGI(TAG, "HttpMp3Player with MCP Tools `self.music.play_url` created.");
    //播放模式設定
    mcp_server.AddTool("self.music.set_play_mode",
            "使用此工具设置对应的播放模式，可以选择单曲播放模式(播放一首后停止)或连续播放模式(持续播放不同歌曲)。\n"
            "参数:\n"
            "  `playmode`: 播放模式，可选值为 'single'(单曲）或 'continuous'（连续）。\n"
            "返回:\n"
            "  设置结果信息。\n"
            "使用規則:\n"
            "  当用户提出'设定播放模式'或类似需求时使用，用户需求可参照范例。\n"
            "范例:\n"
            "  '单曲模式'\n"
            "  '单曲播放'\n"
            "  '设置单曲模式'\n"
            "  '设定单曲模式'\n"
            "  '设置连播模式'\n"
            "  '设定轮播模式'\n"
            "  '连续模式'\n"
            "  '连续播放模式'\n"
            "  '循环模式'\n"
            "  '连播模式'\n",
            PropertyList({
                Property("playmode", kPropertyTypeString)//播放模式: "single" 或 "continuous"
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                auto mode_str = properties["playmode"].value<std::string>();
                // 转换为小写以便比较
                std::transform(mode_str.begin(), mode_str.end(), mode_str.begin(), ::tolower);
                
                if (mode_str == "single" || mode_str == "單曲") {
                    // 設定為單曲播放模式
                    set_play_mode(PlayModeSingle);
                    return "{\"success\": true, \"message\": \"已切换到單曲模式\"}";
                } else if (mode_str == "continuous" || mode_str == "连续" || mode_str == "连播" ||mode_str == "循环") {
                    // 設定為連續播放模式
                    set_play_mode(PlayModeContinuous);
                    return "{\"success\": true, \"message\": \"已切换到连续播放模式。\"}";
                    } else {
                    return "{\"success\": false, \"message\": \"无效的音樂播放模式，请使用 'single' 或 'continuous'\"}";
                    }
                
                return "{\"success\": false, \"message\": \"设置播放模式失败。\"}";
            });
    ESP_LOGI(TAG, "HttpMp3Player with MCP Tools `self.music.set_play_mode` created.");
}

Mp3Player::~Mp3Player(){
    
}

bool Mp3Player::PauseResume() {
    //尚未實作
    return true;
}
bool Mp3Player::Stop() {
    ESP_LOGW(TAG, "嘗試停止播放音樂...");
    if(is_playing_){
        stop_flag_ = true;
    }
    return true;
}
bool Mp3Player::IsPlaying() {
    return is_playing_;
}
#pragma endregion

#pragma region 設定/取得播放模式

bool Mp3Player::set_play_mode(const PlayMode mode){
    play_mode_ = mode;
    Settings settings("mp3_player", true);
    settings.SetInt("playmode", play_mode_);
    return true;
}

PlayMode Mp3Player::get_play_mode(){
    Settings settings("mp3_player", false);
    int value = settings.GetInt("playmode", PlayModeSingle);
    if (value != PlayModeSingle && value != PlayModeContinuous) {  //有可能讀出來是壞的值
        set_play_mode(PlayModeSingle); //強制設定 PlayModeSingle
    }
    else{
        play_mode_ = (PlayMode)value;
    }
    return play_mode_;
}

#pragma endregion

#pragma region 全域函數

std::string Mp3Player::build_stream_url(const std::string &song_id){
    if(mp3_source_ == MP3SourceHTTP){
        return build_stream_url_http(song_id);
    }
    else if (mp3_source_ == MP3SourceFATFS)
    {
        return song_id; //id 就是完整路徑檔名
    }
    
    return "";
}

std::string Mp3Player::build_cover_url(const std::string &cover_id){
    if(mp3_source_ == MP3SourceHTTP){
        return build_cover_url_http(cover_id);
    }
    return "";
}



//印出 Heap 可用記憶體
void print_heap_free_size(){
    int free_sram          = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    int largest_free_block = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    int min_free_sram      = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    ESP_LOGW(TAG, "free sram: %u , largest free block: %u , minimal sram: %u", free_sram, largest_free_block, min_free_sram);
}

/**
 * @brief 計算補滿 ring buffer 時，對應 PCM 資料量的持續時間
 * @note 未傳入值時預設回傳 25
 * 
 * @param out_rb_size    out ring buffer size
 * @param sample_rates   PCM sample rates
 * @param bits           PCM bits
 * @param channels       PCM channels
 *
 * @return uint16_t      回傳毫秒
 */
uint16_t buffer_duration_ms(int out_rb_size = 0, int sample_rates = 0, int bits = 0, int channels = 0){
    int16_t buffer_ms = 25;
    if (sample_rates > 0 && bits > 0 && channels > 0) {
        buffer_ms = uint16_t((out_rb_size * 1000) / (sample_rates * (bits / 8) * channels));
    }
    return (buffer_ms < 50) ? buffer_ms : 50;
}



//解析 url ，賦予 protocol 值 http / https
bool parse_url_protocol(const std::string &url, std::string &protocol){
    protocol.clear();   // 先清空輸出參數
    size_t protocol_end = url.find("://");
    if (protocol_end == std::string::npos) {
        ESP_LOGE(TAG, "Invalid URL format: %s", url.c_str());
        return false;
    }
    protocol = url.substr(0, protocol_end);
    std::transform(protocol.begin(), protocol.end(), protocol.begin(), ::tolower);
    if (protocol != "http" && protocol != "https") {     //支援 http 與 https
        ESP_LOGE(TAG, "Unsupported protocol: %s", protocol.c_str());
        return false;
    }
    return true;
}


#pragma endregion

#pragma region 類別成員-歌單與歌詞
/**
 * 
 * @brief 主要播放入口。藉由歌曲名/歌手名，查詢的結果進行播放
 * 
 * @param song_name      查詢的歌曲名稱
 * @param artist_name    查詢的歌手名稱
 * @param query_result  結果文字，回傳給小智參考
 *
 * @return true         請求成功
 * @return false        請求失敗
 */
bool Mp3Player::QueryAndPlay(const std::string& song_name, const std::string& artist_name, std::string& query_result){
    auto& app = Application::GetInstance();
    auto* display = Board::GetInstance().GetDisplay();
    if(mp3_source_ == MP3SourceUnknow){
        struct stat st;
        if (stat(SD_FLAG_FILE, &st) != 0) {
            if (errno == ENOENT) {
                ESP_LOGE(TAG, "File %s not found", SD_FLAG_FILE);
            } else {
                ESP_LOGE(TAG, "stat failed, errno=%d", errno);
            }
            ESP_LOGW(TAG, "播放 SD 卡 MP3 檔案的條件: 1. 裝置的 SD 或 SDMMC 需正確驅動並初始化。  2. 將 SD 卡以 FAT32 格式化。  3. SD 卡根目錄必須有檔案文件 .xiaozhi-esp32.txt " );
            mp3_source_  = MP3SourceHTTP;
            ESP_LOGW(TAG, "將使用 HTTP / HTTPS 播放 MP3 音樂。" );
        }
        else{
            ESP_LOGW(TAG, "SD 卡掛載成功！播放 SD 卡內的 MP3 音樂。");
            mp3_source_ = MP3SourceFATFS;

            app.Schedule([display, message = Lang::Strings::MUSIC_FROM_SD]() {
                display->SetChatMessage("user", message);
            });
        }
    }
    ESP_LOGI(TAG, "查詢歌手: %s, 歌曲名: %s", artist_name.c_str(), song_name.c_str());
    // 經查詢結果產生歌曲資訊 current_music_info_
    if (!get_music_info(song_name,artist_name)){
        ESP_LOGW(TAG, "create_music_info 錯誤！");
        query_result = "沒有歌曲：【" + song_name + "】，歌手【" + artist_name + "】";
        app.Schedule([display, query_result]() {
            display->SetChatMessage("assistant", query_result.c_str());
        });
        return false;
    }

    if(!get_song_lyrics(current_music_info_.song_id)){
        std::string msg = "【" + current_music_info_.title + "】沒有歌詞!";
        ESP_LOGW(TAG, "%s", msg.c_str());
    }

    ESP_LOGI(TAG, "開始播放歌曲: %s, 歌手: %s", current_music_info_.title.c_str(), current_music_info_.artist.c_str());
    if(play_mode_ == PlayModeContinuous){
        query_result = "開始隨機播放歌曲:《" + current_music_info_.title + "》等 " + std::to_string(playlists_.size()) + " 首歌。";
        //std::string msg = "《找到 " + std::to_string((int)playlists_.size()) + " 首歌》";
        char msg[30];
        snprintf(msg, sizeof(msg), Lang::Strings::NUM_SONGS_FOUND, playlists_.size());
        auto *display = Board::GetInstance().GetDisplay();
        auto &app = Application::GetInstance();
        app.Schedule([display, msg]() {
            display->SetChatMessage("assistant", msg);
        });
        vTaskDelay(pdMS_TO_TICKS(150));
    }
    else{
        query_result = "開始播放歌曲: " + current_music_info_.title + " ,歌手: " + current_music_info_.artist;
    }
    bool success = Play();
    if(!success){
        ESP_LOGE(TAG, "Play() 失敗！");
        query_result = "播放音樂失敗！";
        return false;
    }
    return true;
}

/**
 * @brief 透過查詢條件（歌曲/歌手），建立 current_music_info_ 與 playlists_
 *
 * @param song_name      歌曲名稱
 * @param artist_name    演唱者/藝術家
 *
 * @return true         請求成功
 * @return false        請求失敗
 */
bool Mp3Player::get_music_info(const std::string& song_name, const std::string& artist_name){
    if(mp3_source_ == MP3SourceUnknow){
        ESP_LOGE(TAG,"get_music_info 沒有定義 MP3 來源！");
        return false;
    }
    if(mp3_source_ == MP3SourceHTTP){
        return get_music_info_from_http(song_name,artist_name);
    }
    else if (mp3_source_ == MP3SourceFATFS){
        return get_music_info_from_fatfs(song_name,artist_name);
    }
    
    return false;
    //TODO: 還有 SD 卡
}

/**
 * @brief 根據 song_id 查詢並產生歌詞資料
 *
 * @param song_id       歌曲 id
 *
 * @return true         請求成功
 * @return false        請求失敗
 */
bool Mp3Player::get_song_lyrics(const std::string& song_id){
    if(mp3_source_ == MP3SourceUnknow){
        ESP_LOGE(TAG,"get_song_lyrics 沒有定義 MP3 來源！");
        return false;
    }
    if(mp3_source_ == MP3SourceHTTP){
        return get_song_lyrics_from_http(song_id);
    }
    else if (mp3_source_ == MP3SourceFATFS)
    {
        return get_song_lyrics_from_fatfs(song_id);
    }
    return false;
}


/**
 * @brief 從 playlists_ 中亂數取用一首到 current_music_info_
 * 
 * @return true         請求成功
 * @return false        請求失敗
 */
bool Mp3Player::random_choose_song(){
    size_t song_count = playlists_.size();
    if(song_count <= 0){
        ESP_LOGE(TAG, "playlists_ : No playlists available");
        return false;
    }

    try {
        //1. 當 current_music_info_.song_id.empty() 時表示 current_music_info_ 內容為初始空值。
        //這時直接從 playlists_ 隨機選一首賦值給 current_music_info_
        //2. 當 current_music_info_.song_id 已經有資料時，current_music_info_ 很可能是前一首剛播放完。
        //這時：若 playlists_ 兩首以上，則不要重複挑到同一首。
        //     若 playlists_ 只有 1 首，則不變更 current_music_info_
        int16_t index = -1;
        if(current_music_info_.song_id.empty()){ //當前歌曲空白，從 playlists_ 亂數取一首
            uint32_t r = esp_random();
            index = r % song_count;
        }
        else{ //current_music_info_ 已經有資料，可能是剛播放完的
            std::string next_song_id = current_music_info_.song_id;
            while (song_count >= 2 && next_song_id == current_music_info_.song_id) //避免下一首挑到同一首歌
            {
                uint32_t r = esp_random();   // 硬體亂數
                index = r % song_count;
                next_song_id = playlists_.at(index).song_id;
            }
        }
        if(index >=0 && index < song_count){ //選出的歌曲 index 值
            current_music_info_.song_id = playlists_.at(index).song_id;
            current_music_info_.title  = playlists_.at(index).title;
            current_music_info_.artist = playlists_.at(index).artist;
            current_music_info_.cover_id = playlists_.at(index).cover_id;
            current_music_info_.sampling_rate = playlists_.at(index).sampling_rate;
            current_music_info_.channel_count = playlists_.at(index).channel_count;
            //清空歌詞資料
            current_music_info_.lyrics.clear();
            current_music_info_.lyrics.shrink_to_fit();
        }
        return true;
    } catch (const std::out_of_range& e) {
        ESP_LOGE(TAG, "Out of range: %s", e.what() );
        return false;
    }  
}

#pragma endregion

#pragma region 取得專輯封面

#if defined(CONFIG_SPIRAM)

bool Mp3Player::get_cover_by_coverid(const std::string& cover_id, uint8_t** out_buf, size_t* out_size){
    if(mp3_source_ == MP3SourceHTTP){
        return get_cover_by_coverid_http(cover_id,out_buf,out_size);
    }
    else if(mp3_source_ == MP3SourceFATFS){
        return get_cover_by_coverid_fatfs(cover_id,out_buf,out_size);
    }
    return false;
}

#include "lcd_display.h"
#include "esp_lv_decoder.h"
//圖片解碼
esp_lv_decoder_handle_t decoder_handle_ = NULL;

void Mp3Player::show_cover_by_coverid(const std::string& cover_id) {
#if !defined(CONFIG_SPIRAM) 
    return;
#endif
    try {
        auto display = dynamic_cast<LvglDisplay*>(Board::GetInstance().GetDisplay());
        if(display){
            uint8_t* cover_buf = nullptr;
            size_t cover_size = 0;
            if (!get_cover_by_coverid(cover_id, &cover_buf, &cover_size)) {
                ESP_LOGE(TAG, "Failed to get cover");
                return;
            }
            if (decoder_handle_ == NULL) {
                esp_err_t ret = esp_lv_decoder_init(&decoder_handle_);
                if (ret != ESP_OK) {
                    ESP_LOGE(TAG, "Failed to initialize ESP LVGL decoder (%s)", esp_err_to_name(ret));
                } else {
                    ESP_LOGI(TAG, "ESP LVGL decoder initialized");
                }
            }
            //方法1. 使用 LVGL 內建解碼器，需要
            //#include "esp_lv_decoder.h"
            //   esp_lv_decoder_handle_t decoder_handle_ = NULL;
            //   <....lv_init();....> //在 lv_init() 之後才 esp_lv_decoder_init
            //   esp_err_t ret = esp_lv_decoder_init(&decoder_handle_);

            auto cover_img = std::make_unique<LvglAllocatedImage>(cover_buf, cover_size);
            display->SetPreviewImage(std::move(cover_img));
        }
    }
    catch (const std::bad_alloc& e) {
        ESP_LOGE(TAG, "Memory allocation failed: %s", e.what());
    }
    catch (const std::exception& e) {
        ESP_LOGE(TAG, "Exception: %s", e.what());
    }
    catch (...) {
        ESP_LOGE(TAG, "Unknown exception occurred");
    }
}

#endif

#pragma endregion

#pragma region 播放 Play 相關

/**
 * @brief 連續播放模式
 * 
 */
void Mp3Player::continuous_playing(){
    int song_count = playlists_.size();
    if (song_count <= 0){
        ESP_LOGE(TAG, "playlists_ : No playlists available");
        return;
    }

    if(!random_choose_song()){
        ESP_LOGE(TAG, "random_choose_song failed");
        return;
    }
    //取得歌詞
    current_music_info_.lyrics.clear();
    current_music_info_.lyrics.shrink_to_fit();
    if(!get_song_lyrics(current_music_info_.song_id)){
        std::string msg = "【" + current_music_info_.title + "】沒有歌詞!";
        ESP_LOGW(TAG,"%s",msg.c_str());
    }
    Play();
}

bool Mp3Player::Play()
{
    if(!current_music_info_.song_id.empty()){
        current_music_info_.mp3_url = build_stream_url(current_music_info_.song_id);
    }
    if(current_music_info_.mp3_url.empty()) {
        ESP_LOGE(TAG, "_current_music_info.mp3_url.empty()");
        return false;
    }
    /* 語音會話期間內部 RAM 僅剩數 KB（WakeNet+AFE+Opus+MQTT 佔用），
       直接建立 8KB 播放任務會因記憶體不足而失敗。
       先建立輕量排程任務（小棧），等語音會話結束、記憶體回升後再建立真正的播放任務。 */
    const BaseType_t core_id = CONFIG_FREERTOS_NUMBER_OF_CORES - 1;
    BaseType_t result = xTaskCreatePinnedToCore(
        play_scheduler_task,
        "PLAY_SCHED",
        2560,
        this,
        pipeline_task_prio_ - 1,
        nullptr,
        core_id
    );

    if (result != pdPASS) {
        ESP_LOGE(TAG, "Failed to create play scheduler task, free internal=%u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        return false;
    }

    return true; // 立即回傳
}

void Mp3Player::play_scheduler_task(void* arg)
{
    auto* self = static_cast<Mp3Player*>(arg);
    auto& app = Application::GetInstance();

    // 等待小智說完話（speaking/connecting -> listening）
    while (app.GetDeviceState() == kDeviceStateSpeaking ||
           app.GetDeviceState() == kDeviceStateConnecting) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    // 強制結束會話（listening -> idle），釋放 AFE / Opus / MQTT 佔用的內部記憶體
    while (app.GetDeviceState() == kDeviceStateListening) {
        ESP_LOGI(TAG, "切換至待機狀態，釋放記憶體後開始播放...");
        app.ToggleChatState();
        vTaskDelay(pdMS_TO_TICKS(300));
    }

    // 會話結束後內部記憶體回升，建立播放任務
    // 歌詞查詢（HTTP+JSON 深呼叫棧）已移至 STREAM_TASK 內執行：
    // 排程任務僅 2.5KB 棧，先前在 此查歌詞導致 stack overflow、設備重啟
    const BaseType_t core_id = CONFIG_FREERTOS_NUMBER_OF_CORES - 1;
    BaseType_t result = pdFAIL;
    for (int i = 0; i < 10 && result != pdPASS; i++) {
        result = xTaskCreatePinnedToCore(
            streaming_task,
            "STREAM_TASK",
            10240,
            self,
            pipeline_task_prio_, //優先權，0~24，越大越優先，要實際測試過才知道，太大會與 WIFI 搶資源，太小會斷音
            nullptr,
            core_id
        );
        if (result != pdPASS) {
            ESP_LOGW(TAG, "STREAM_TASK 建立失敗（free internal=%u），等待重試...",
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
            vTaskDelay(pdMS_TO_TICKS(500));
        }
    }
    if (result != pdPASS) {
        ESP_LOGE(TAG, "Failed to create streaming task after retries");
    }
    vTaskDelete(nullptr);
}

void Mp3Player::streaming_task(void* arg)
{
    auto* self = static_cast<Mp3Player*>(arg);

    vTaskDelay(pdMS_TO_TICKS(300));

    // 會話已結束、內部記憶體回升：在此查詢歌詞（HTTP+JSON 需要較深呼叫棧，
    // 10KB 棧足夠；查詢完成後棧幀釋放，不影響後續串流管線的棧用量）
    if (!self->current_music_info_.song_id.empty()) {
        if (!self->get_song_lyrics(self->current_music_info_.song_id)) {
            ESP_LOGW(TAG, "【%s】沒有歌詞!", self->current_music_info_.title.c_str());
        } else {
            ESP_LOGI(TAG, "已載入歌詞 %d 行", (int)self->current_music_info_.lyrics.size());
        }
    }

    self->start_streaming_pipeline();

    vTaskDelete(nullptr);
}

/**
 * @brief 使用 ESP-ADF 的 audio pipeline 進行 current_music_info_ 播放，
 * 
 * @return true         請求成功
 * @return false        請求失敗
 */
bool Mp3Player::start_streaming_pipeline(){
    #warning "經實測驗證，audio pipeline 支援 ESP32-S3 / C5 / C6 ，不支援最早的 ESP32"

    if (current_music_info_.mp3_url.empty())
    {
        ESP_LOGE(TAG, "Music URL is empty");
        return false;
    }
    if(mp3_source_ == MP3SourceUnknow){
        ESP_LOGE(TAG, "start_streaming_pipeline: MP3 來源尚未定義！");
        return false;
    }
    std::string protocol_ = ""; //賦與值 http 或 https
    if(mp3_source_ == MP3SourceHTTP){
        if (!parse_url_protocol(current_music_info_.mp3_url,protocol_)){
            ESP_LOGE(TAG, "parse_url_protocol error! 不支援的網址！");
            return false;
        }
    }

    auto &app = Application::GetInstance();
    ESP_LOGW(TAG, "等待小智囉唆完畢....");
    while(app.GetDeviceState() == kDeviceStateSpeaking || 
          app.GetDeviceState() == kDeviceStateConnecting){
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    while(app.GetDeviceState() == kDeviceStateListening){
        ESP_LOGI(TAG, "切換至待機狀態，以免小智邊播歌邊插嘴...");
        app.ToggleChatState();
        vTaskDelay(pdMS_TO_TICKS(300));
    }

    is_playing_ = true;
    stop_flag_ = false;

    //如果有語音喚醒功能，播放音樂時關閉
    bool is_wake_word_running = app.GetAudioService().IsWakeWordRunning();
    if(is_wake_word_running){
        app.GetAudioService().EnableWakeWordDetection(false);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    auto led = dynamic_cast<SingleLed*>(Board::GetInstance().GetLed());
    if(led){
        led->SetColor(0,DEFAULT_BRIGHTNESS,DEFAULT_BRIGHTNESS);
        led->TurnOn();
    }

    //裝置已待命（會話記憶體已釋放供播放管線使用）：狀態欄改顯示「播放中」，避免使用者誤認為停止播放
    {
        auto status_display = Board::GetInstance().GetDisplay();
        app.Schedule([status_display]() {
            status_display->SetStatus(Lang::Strings::MUSIC_PLAYING);
        });
    }

#ifdef CONFIG_SPIRAM
    show_cover_by_coverid(current_music_info_.cover_id);
#endif
    
    auto &board = Board::GetInstance();
    board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE); //避免待機時 WIFI 進入低功耗導致網路降速
    vTaskDelay(pdMS_TO_TICKS(200));

    // 定義管線 ring buffer 大小，http - mp3 - raw 建議由大至小，不然可能會餵不飽後面的 rb_size
#if defined(CONFIG_SPIRAM)
    const size_t mp3source_out_rb_size = 20 * 1024;
    const size_t mp3_out_rb_size = 16 * 1024;
    const size_t raw_out_rb_size = 16 * 1024;
#else
    const size_t mp3source_out_rb_size = 12 * 1024;
    const size_t mp3_out_rb_size = 4 * 1024;
    const size_t raw_out_rb_size = 4 * 1024;
#endif
    ESP_LOGW(TAG, "開始 HTTP/FATFS - MP3 - RAW 音樂管線流程....");
    ESP_LOGI(TAG, "代碼薅自 ESP-ADF 範例 pipeline_http_mp3 再做些修改...");
    
    ESP_LOGI(TAG, "[1.0] Prepare pipeline and stream.");
    audio_pipeline_handle_t pipeline;
    audio_element_handle_t mp3source_stream_reader, mp3_decoder, raw_stream_reader;

    ESP_LOGI(TAG, "[2.0] Create audio pipeline for playback");
    audio_pipeline_cfg_t pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
    pipeline = audio_pipeline_init(&pipeline_cfg);
    mem_assert(pipeline);

    if(mp3_source_ == MP3SourceFATFS){
        ESP_LOGI(TAG, "[2.1] Create fatfs stream to get data");
        fatfs_stream_cfg_t fatfs_cfg = FATFS_STREAM_CFG_DEFAULT();
        fatfs_cfg.type = AUDIO_STREAM_READER;
        fatfs_cfg.out_rb_size = mp3source_out_rb_size;
        fatfs_cfg.task_prio = 1; //pipeline_task_prio_;
        mp3source_stream_reader = fatfs_stream_init(&fatfs_cfg);
    }
    else{ //mp3_source_ == MP3SourceHTTP
        ESP_LOGI(TAG, "[2.1] Create http stream to get data");
        http_stream_cfg_t http_cfg = HTTP_STREAM_CFG_DEFAULT();
        http_cfg.out_rb_size = mp3source_out_rb_size;
        http_cfg.task_prio = pipeline_task_prio_; //解決 ESP32 C5 出現看門狗警告、下載卡頓。
        if (protocol_ == "https") {
            //Subsonic API 採用 HTTPS，CPU 負載提高且需更大緩衝區。
            ESP_LOGW(TAG, "[2.1.1] Subsonic API uses HTTPS/TLS, increasing CPU load and requiring larger buffers for stable playback");
            http_cfg.crt_bundle_attach = esp_crt_bundle_attach;
#if defined(CONFIG_SPIRAM)
            //具備 PSRAM，可擴大 TCP 緩衝與 ring buffer，改善 HTTPS 播放順暢度。
            ESP_LOGI(TAG, "[2.1.2] PSRAM available: increasing TCP request size and ringbuffer for smoother HTTPS playback");
            http_cfg.request_size = 32 * 1024;
            http_cfg.out_rb_size  = 64 * 1024;
#endif
        }
        mp3source_stream_reader = http_stream_init(&http_cfg);    
    }
    
    ESP_LOGI(TAG, "[2.2] Create mp3 decoder to decode mp3 data");
    mp3_decoder_cfg_t mp3_cfg = DEFAULT_MP3_DECODER_CONFIG();
    mp3_cfg.out_rb_size = mp3_out_rb_size ; //原本只有 2K 有偶發斷音的現象
    mp3_cfg.task_prio = pipeline_task_prio_;
    mp3_decoder = mp3_decoder_init(&mp3_cfg);

    ESP_LOGI(TAG, "[2.3] Create raw stream reader to read data from pipeline then write to codec chip");
    raw_stream_cfg_t raw_cfg = RAW_STREAM_CFG_DEFAULT();
    raw_cfg.type = AUDIO_STREAM_READER;
    raw_cfg.out_rb_size = raw_out_rb_size;
    raw_stream_reader = raw_stream_init(&raw_cfg);

    ESP_LOGI(TAG, "[2.4] Register all elements to audio pipeline");
    audio_pipeline_register(pipeline, mp3source_stream_reader, "http_fatfs");
    audio_pipeline_register(pipeline, mp3_decoder,        "mp3");
    audio_pipeline_register(pipeline, raw_stream_reader,  "raw");

    ESP_LOGI(TAG, "[2.5] Link it together http/fatfs_stream-->mp3_decoder-->raw_stream");
    const char *link_tag[3] = {"http_fatfs", "mp3", "raw"};
    audio_pipeline_link(pipeline, &link_tag[0], 3);

    ESP_LOGI(TAG, "[2.6] Set up  uri");
    audio_element_set_uri(mp3source_stream_reader, current_music_info_.mp3_url.c_str());

    ESP_LOGI(TAG, "[ 3 ] Set up  event listener");
    audio_event_iface_cfg_t evt_cfg = AUDIO_EVENT_IFACE_DEFAULT_CFG();
    audio_event_iface_handle_t evt = audio_event_iface_init(&evt_cfg);

    ESP_LOGI(TAG, "[3.1] Listening event from all elements of pipeline");
    audio_pipeline_set_listener(pipeline, evt);

    ESP_LOGI(TAG, "[3.2] Prepare anything for playback control");
    
    //取得小智 AI 的全域 codec
    auto codec = board.GetAudioCodec();
    
    if (!codec->output_enabled())
    {
        codec->EnableOutput(true);
    }
    //取得小智 AI 的屏幕物件
    auto display = board.GetDisplay();

    if((int)current_music_info_.lyrics.size() == 0){
        std::string msg = "《" + current_music_info_.title + "》沒有歌詞。";
        app.Schedule([display, msg]() {
            display->SetChatMessage("assistant", msg.c_str());
        });
    }
    app.Schedule([display]() {
        display->SetEmotion("happy");
    });
    
    // PCM 數據參數
    constexpr size_t PCM_BYTES = 2048;
    int16_t pcm_buf[PCM_BYTES / sizeof(int16_t)];
    std::vector<int16_t> pcm_data;
    pcm_data.reserve(PCM_BYTES / sizeof(int16_t));

    //參數：計算 codec->SetOutputSampleRate 與歌詞時間的參數
    int sample_rates = 44100; // 當前 MP3 的採樣頻率
    int bits = 16;
    int channels = 1;
    //ring buffer 的緩衝時間
    uint16_t pcm_buffer_duration_ms = buffer_duration_ms();

    //參數：播放進度推算歌詞的位置
    size_t total_frames_played = 0; //已播放的音頻 Frame 幀數，用來換算播放時間
    size_t current_lyric_index = 0; //當前歌詞的位置

    bool complete_played = false; //判定是中斷或正常播完

    //預緩存機制
    bool buffer_enabled = true; //是否啟用預緩存

    //診斷統計：追蹤 PCM 讀取與 I2S 寫入（排查無聲問題）
    size_t diag_bytes_read = 0;        // raw_stream_read 累計位元組
    size_t diag_samples_written = 0;   // 送入 codec 的累計樣本
    bool diag_first_block = true;      // 首塊 PCM 采样打印
    TickType_t diag_last_beat = xTaskGetTickCount();

    ESP_LOGI(TAG, "[ 4 ] Start audio_pipeline");
    audio_pipeline_run(pipeline);

    while (!stop_flag_) {
        // 1. 同步事件檢查 pipeline stop 或 music info
        audio_event_iface_msg_t msg;
        while (audio_event_iface_listen(evt, &msg, 0) == ESP_OK) { // 非阻塞
            //取得 MP3 的規格資訊
            if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT &&
                msg.source == (void *) mp3_decoder &&
                msg.cmd == AEL_MSG_CMD_REPORT_MUSIC_INFO) {
                    audio_element_info_t music_info = {0};
                    audio_element_getinfo(mp3_decoder, &music_info);
                    ESP_LOGI(TAG, "[ * ] Receive music info from mp3 decoder, sample_rates=%d, bits=%d, ch=%d",
                    music_info.sample_rates, music_info.bits, music_info.channels);
                    channels = (int)music_info.channels;
                    sample_rates = music_info.sample_rates;
                    bits = music_info.bits;
                    pcm_buffer_duration_ms = buffer_duration_ms(raw_out_rb_size, sample_rates, bits, channels );
                    ESP_LOGW(TAG, "pcm_buffer_duration_ms = %d", pcm_buffer_duration_ms);
                    print_heap_free_size();
                    codec->SetOutputSampleRate(music_info.sample_rates, (channels == 2 && support_stereo_));
                    print_heap_free_size();
            }
            //取得 HTTP 管線的進度
            if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT &&
                msg.source == (void *) mp3source_stream_reader &&
                msg.cmd == AEL_MSG_CMD_REPORT_POSITION) {
                    audio_element_info_t http_info = {0};
                    audio_element_getinfo(mp3source_stream_reader,&http_info);
                    ESP_LOGI(TAG, "[ * ] http current position %d", http_info.byte_pos);                    

            }
            // 加入對 mp3_decoder 狀態的監聽
            if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT &&
                msg.source == (void *) mp3_decoder && 
                msg.cmd == AEL_MSG_CMD_REPORT_STATUS && 
                ((int)msg.data == AEL_STATUS_STATE_STOPPED || (int)msg.data == AEL_STATUS_STATE_FINISHED)) {
                    ESP_LOGW(TAG, "mp3_decoder (AEL_STATUS_STATE = %d)，切換至強迫輸出模式...", msg.data);
                    // 關鍵：一旦解碼器結束，就強制解除 buffer_enabled 狀態
                    buffer_enabled = false;
            }
            //正常播完
            if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT &&
                msg.source == (void *) raw_stream_reader &&
                msg.cmd == AEL_MSG_CMD_REPORT_STATUS &&
                ((int)msg.data == AEL_STATUS_STATE_STOPPED || (int)msg.data == AEL_STATUS_STATE_FINISHED)) {
                    ESP_LOGW(TAG, "raw_stream_reader (AEL_STATUS_STATE = %d)，正常結束...", msg.data);
                    complete_played = true;
                    stop_flag_ = true;
                    break;
            }
        }

        // 2. 緩衝機制
        if (buffer_enabled){
            // 2.1 檢查 mp3_decoder 輸出端目前累積了多少資料
            //ringbuf_handle_t out_rb = audio_element_get_output_ringbuf(mp3_decoder);
            //int fill_level = rb_bytes_filled(out_rb);
            // 2.1 檢查 Raw Stream 輸入端目前累積了多少資料
            ringbuf_handle_t in_rb = audio_element_get_input_ringbuf(raw_stream_reader);
            int fill_level = rb_bytes_filled(in_rb);
            // 2.2. 如果解碼器還在跑，才執行補水邏輯；如果解碼器停了，就直接往下走。
            // 只有在「真的乾了(fill_level < 4 * 1024)」或是「正在補水且還沒補滿(is_buffering)」時才停下來
            if(fill_level < ((4 * 1024 < raw_out_rb_size / 2) ? (4 * 1024) : (raw_out_rb_size / 2)) ){
                int free_sram          = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
                int largest_free_block = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
                if (fill_level > 0) {
                    ESP_LOGI(TAG, "(%d)預緩衝中... (%d/%d) : free sram - %u, largest free block - %u ", pcm_buffer_duration_ms, 
                            fill_level, raw_out_rb_size, free_sram, largest_free_block );
                }
                /*PCM 小教室
                44100 Hz / 16-bit / 立體聲 的資料量：
                   1 個 frame 資料量為：左聲 16 bit = 2 bytes + 右聲 16bit = 2 bytes = 4 bytes。
                   1 秒 44100 frame ≈ 172.3 KB/s (44100 × 4)
                   假設 raw_out_rb_size = 4KB，
                   那麼 4KB 資料量相當於 4,096 / 176,400 ≈ 0.023 秒（23ms）
                   緩衝時間可用這個數字當參考
                */
                vTaskDelay(pdMS_TO_TICKS(pcm_buffer_duration_ms));
                continue;
            }
        }
        try {
            // 3. 讀出管線中的 PCM 數據。mp3_decoder 已經幫忙處理成 PCM 數據，
            int read_len = raw_stream_read(raw_stream_reader, reinterpret_cast<char *>(pcm_buf), PCM_BYTES);
            if (read_len > 0) {
                diag_bytes_read += read_len;
                if (diag_first_block) {
                    diag_first_block = false;
                    ESP_LOGW(TAG, "首塊PCM: len=%d 樣本=[%d %d %d %d] %s",
                             read_len, pcm_buf[0], pcm_buf[1], pcm_buf[2], pcm_buf[3],
                             (pcm_buf[0] == 0 && pcm_buf[1] == 0) ? "⚠️全零=靜音數據" : "✓非零");
                }
                size_t num_samples = read_len / sizeof(int16_t); // 總樣本數
                size_t frames = num_samples / channels; //總 frame 數，計算歌詞用
                //size_t channels = music_info.channels;          // mp3 decoder 的 channel 數

                if (channels == 2 && !support_stereo_) { //不支援立體聲時，則 L+R 混音
                    // stereo -> mono
                    size_t mono_samples = num_samples / channels; 
                    for (size_t i = 0; i < mono_samples; ++i) {
                        int16_t left  = pcm_buf[2*i];
                        int16_t right = pcm_buf[2*i + 1];
                        pcm_buf[i] = (left / 2 + right / 2); // 混合成 mono
                    }
                    num_samples = mono_samples;
                    frames = mono_samples;
                }
                // 開始計算時間，準備同步歌詞
                // 累計樣本數，以原本的 num_samples 為計算基準而不是單聲道 mono_samples
                total_frames_played += frames;

                if((int)current_music_info_.lyrics.size() > 0){
                    // ===== 播歌時同步歌詞 =====
                    // 計算目前播放時間（毫秒）
                    uint32_t current_ms = (uint64_t)total_frames_played * 1000 / sample_rates;
                    size_t lyric_count = current_music_info_.lyrics.size();
                    while (current_lyric_index < lyric_count &&
                        current_ms >= current_music_info_.lyrics[current_lyric_index].start_ms) {
                        //display_lyric(_current_music_info.lyrics[_current_lyric_index].text);
                        //ESP_LOGI(TAG, "%s", current_music_info_.lyrics[current_lyric_index].text );
                        app.Schedule([display , message = current_music_info_.lyrics[current_lyric_index].text ]() {
                            display->SetChatMessage("assistant", message);
                        });
                        current_lyric_index++;
                    }
                }
                // 計算秒數 保留這段寫法
                //float seconds_played = (float)total_frames_played / sample_rates;
                //int minutes = (int)(seconds_played / 60);
                //int seconds = (int)(seconds_played) % 60;

                // 日誌 / 外部可讀
                //ESP_LOGI(TAG, "播放進度 %02d:%02d", minutes, seconds);

                // 將 PCM 放入 vector
                pcm_data.assign(pcm_buf, pcm_buf + num_samples);

                // 送給 codec
                codec->OutputData(pcm_data);
                diag_samples_written += num_samples;

                // 15 秒心跳：確認播放循環活著、PCM 持續寫入 I2S
                TickType_t now = xTaskGetTickCount();
                if (now - diag_last_beat >= pdMS_TO_TICKS(15000)) {
                    diag_last_beat = now;
                    uint32_t played_s = (uint32_t)((uint64_t)total_frames_played * 1000 / sample_rates / 1000);
                    ESP_LOGW(TAG, "♪心跳: 已播%us 已讀%uKB 已寫I2S %u樣本(約%us@%dHz) 歌詞%u/%u",
                             played_s, (unsigned)(diag_bytes_read / 1024), (unsigned)diag_samples_written,
                             (unsigned)((uint64_t)diag_samples_written / sample_rates), sample_rates,
                             (unsigned)current_lyric_index, (unsigned)current_music_info_.lyrics.size());
                }
            }

        } catch(const std::exception &e) {
            ESP_LOGE(TAG, "Exception: %s", e.what());
            stop_flag_ = true; // 安全停止
        }
        vTaskDelay(pdMS_TO_TICKS(3));
    }

    ESP_LOGI(TAG, "[ 5 ] Stop audio_pipeline");
    //播放結束診斷統計：判斷 PCM 是否真的送達 I2S
    ESP_LOGW(TAG, "播放統計: 讀取%uKB PCM, 寫入I2S %u樣本(約%us@%dHz), 完整播放=%s",
             (unsigned)(diag_bytes_read / 1024), (unsigned)diag_samples_written,
             (unsigned)((uint64_t)diag_samples_written / (sample_rates ? sample_rates : 1)),
             sample_rates, complete_played ? "是" : "否");
    audio_pipeline_stop(pipeline);
    audio_pipeline_wait_for_stop(pipeline);
    //vTaskDelay(pdMS_TO_TICKS(30));  // ⭐ 再等一下確保 element task 真正退出，播免後續資源清除不乾淨，例如監聽物件仍在作用，導致崩潰錯誤
    audio_pipeline_terminate(pipeline);
    //audio_pipeline_wait_for_stop(pipeline);   // 再一次保險

    /* Terminate the pipeline before removing the listener */
    audio_pipeline_unregister(pipeline, mp3source_stream_reader);
    audio_pipeline_unregister(pipeline, raw_stream_reader);
    audio_pipeline_unregister(pipeline, mp3_decoder);

    audio_pipeline_remove_listener(pipeline);

    /* Make sure audio_pipeline_remove_listener & audio_event_iface_remove_listener are called before destroying event_iface */
    audio_event_iface_destroy(evt);

    /* Release all resources */
    audio_pipeline_deinit(pipeline);
    audio_element_deinit(mp3source_stream_reader);
    audio_element_deinit(raw_stream_reader);
    audio_element_deinit(mp3_decoder);

    codec->ResetOutputSampleRate(); //恢復原來的設定

    board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER); //恢復待機時 WIFI 低功耗

    //播放結束：裝置若仍在待命（未開新會話），狀態欄恢復「待命」；若已開新會話則由狀態機自動覆蓋
    app.Schedule([&app, display]() {
        if (app.GetDeviceState() == kDeviceStateIdle) {
            display->SetStatus(Lang::Strings::STANDBY);
        }
    });

    //當完整播完歌，且為連續播放模式時保留 current_music_info_ 用來判定下一首不要重複，否則清空
    if(!(complete_played && play_mode_ == PlayModeContinuous)){
        current_music_info_ = MusicInfo{}; //重置內容
    }

    is_playing_ = false;

    //如果本來有語音喚醒功能，音樂停止時恢復啟用
    if(is_wake_word_running && !app.GetAudioService().IsWakeWordRunning() ){
        app.GetAudioService().EnableWakeWordDetection(true);
    }
    if(led){
        led->TurnOff();
    }

    if(complete_played){
        app.Schedule([display, message = Lang::Strings::MUSIC_FINISHED]() {
            display->SetChatMessage("assistant", message);
        });
        vTaskDelay(pdMS_TO_TICKS(300)); //讓對話的畫面滑順一點
        if(app.GetDeviceState() == kDeviceStateIdle){
            if(play_mode_ == PlayModeSingle){
                ESP_LOGW(TAG,"單曲模式");
                app.ToggleChatState();
            }
            else if(play_mode_ == PlayModeContinuous){
                ESP_LOGW(TAG,"連播模式");
                continuous_playing();
            }
        }
    }
    else{
        app.Schedule([display, message = Lang::Strings::MUSIC_STOPPED]() {
            display->SetChatMessage("user", message);
        });
    }

    return true;
}

#pragma endregion