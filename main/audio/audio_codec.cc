#include "audio_codec.h"
#include "board.h"
#include "settings.h"

#include <esp_log.h>
#include <cstring>
#include <driver/i2s_common.h>

#define TAG "AudioCodec"

AudioCodec::AudioCodec() {
}

AudioCodec::~AudioCodec() {
}

void AudioCodec::OutputData(std::vector<int16_t>& data) {
    Write(data.data(), data.size());
}

bool AudioCodec::InputData(std::vector<int16_t>& data) {
    int samples = Read(data.data(), data.size());
    if (samples > 0) {
        return true;
    }
    return false;
}

void AudioCodec::Start() {
    Settings settings("audio", false);
    output_volume_ = settings.GetInt("output_volume", output_volume_);
    if (output_volume_ <= 0) {
        ESP_LOGW(TAG, "Output volume value (%d) is too small, setting to default (10)", output_volume_);
        output_volume_ = 10;
    }

    ESP_LOGI(TAG, "Audio codec started, output volume: %d", output_volume_);
}

void AudioCodec::SetOutputVolume(int volume) {
    output_volume_ = volume;
    ESP_LOGI(TAG, "Set output volume to %d", output_volume_);
    
    Settings settings("audio", true);
    settings.SetInt("output_volume", output_volume_);
}

void AudioCodec::SetInputGain(float gain) {
    input_gain_ = gain;
    ESP_LOGI(TAG, "Set input gain to %.1f", input_gain_);
}

void AudioCodec::EnableInput(bool enable) {
    if (enable == input_enabled_) {
        return;
    }
    input_enabled_ = enable;
    ESP_LOGI(TAG, "Set input enable to %s", enable ? "true" : "false");
}

void AudioCodec::EnableOutput(bool enable) {
    if (enable == output_enabled_) {
        return;
    }
    output_enabled_ = enable;
    ESP_LOGI(TAG, "Set output enable to %s", enable ? "true" : "false");
}

bool AudioCodec::SetOutputSampleRate(uint32_t sample_rate, bool enable_strero) {    
    if (sample_rate <= 0 || sample_rate > 192000) {
        ESP_LOGE(TAG, "Invalid sample rate: %d", sample_rate);
        return false;
    }
      
    if (tx_handle_ == nullptr) {
        ESP_LOGE(TAG, "TX handle is null");
        return false;
    }
    
    ESP_LOGI(TAG, "Changing output sample rate from %d to %d Hz", output_sample_rate_, sample_rate);

    //先比對 original_std_cfg_ 有沒有重新指定
    i2s_std_config_t zero_cfg = {};
    if (memcmp(&original_std_tx_cfg_, &zero_cfg, sizeof(i2s_std_config_t)) == 0) {
        ESP_LOGE(TAG, "`%s` in codec_XX.cc is not configured; aborting I2S reconfigure.", "original_std_tx_cfg_ = std_cfg;");
        return false;
    }
    
    // 先尝试禁用 I2S 通道（如果已启用的话）
    esp_err_t disable_ret = i2s_channel_disable(tx_handle_);
    if (disable_ret == ESP_OK) {
        ESP_LOGI(TAG, "Disabled I2S TX channel for reconfiguration");
    } else if (disable_ret == ESP_ERR_INVALID_STATE) {
        // 通道可能已经是禁用状态，这是正常的
        ESP_LOGI(TAG, "I2S TX channel was already disabled");
    } else {
        ESP_LOGE(TAG, "Failed to disable I2S TX channel: %s", esp_err_to_name(disable_ret));
        return false;
    }

    i2s_std_config_t new_std_cfg = original_std_tx_cfg_;
    //變更 I2S Sample Rate
    new_std_cfg.clk_cfg.sample_rate_hz = sample_rate;  
    esp_err_t ret_clk = i2s_channel_reconfig_std_clock(tx_handle_, &new_std_cfg.clk_cfg);
    if (ret_clk != ESP_OK) {
        ESP_LOGE(TAG, "Failed to change sample rate to %d Hz: %s", sample_rate, esp_err_to_name(ret_clk));
        return false;
    } else {
        ESP_LOGI(TAG, "Successfully changed output sample rate to %d Hz", sample_rate);        
    }

    //變更 I2S 的 Slot 設定，判定是否為立體聲
    new_std_cfg.slot_cfg.slot_mode = (enable_strero) ? I2S_SLOT_MODE_STEREO : I2S_SLOT_MODE_MONO;
    esp_err_t ret_slot = i2s_channel_reconfig_std_slot(tx_handle_, &new_std_cfg.slot_cfg);
    if (ret_slot != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable %s mode: %s", (enable_strero) ? "STEREO" : "MONO", esp_err_to_name(ret_slot));
        return false;
    } else {
        ESP_LOGI(TAG, "Successfully enabled %s mode.", (enable_strero) ? "STEREO" : "MONO");
    }
    
    // 重新启用通道（无论之前是什么状态，现在都需要启用以便播放音频）
    esp_err_t enable_ret = i2s_channel_enable(tx_handle_);
    if (enable_ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable I2S TX channel: %s", esp_err_to_name(enable_ret));
        return false;
    } else {
        ESP_LOGI(TAG, "Enabled I2S TX channel");
    }
    return true;
}

bool AudioCodec::ResetOutputSampleRate(){
    if(original_std_tx_cfg_.clk_cfg.sample_rate_hz > 0){
        output_sample_rate_ = original_std_tx_cfg_.clk_cfg.sample_rate_hz;
    }
    return SetOutputSampleRate(output_sample_rate_);
}