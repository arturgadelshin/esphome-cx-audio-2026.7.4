#include "cx_i2s.h"
#include "cx_audio.h"
#include "esphome/core/log.h"
#include "esphome/core/helpers.h"
#include "esphome/core/hal.h"
#include <cstdio>
#include <driver/i2s.h>
#include <driver/gpio.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <esp_task_wdt.h>
#include <esp_timer.h>

extern "C" {
#include <va_dsp.h>
#include <cnx20921_init.h>
int cx20921SetMicGain(int gain_db);
}

namespace esphome::cx_i2s {

static const char *const TAG = "cx_i2s";

extern "C" void esphome_set_dsp_fw_mode(int mode);

void CXI2SMicrophone::setup() {
  ESP_LOGI(TAG, "Setting up CX I2S Microphone...");

  bool use_fw = this->parent_->is_use_firmware();
  int fw_mode = use_fw ? 1 : 0;
  ESP_LOGI(TAG, "DSP Init Config: use_firmware=%s, mode=%d", use_fw ? "YES" : "NO", fw_mode);

  esphome_set_dsp_fw_mode(fw_mode);

  ESP_LOGI(TAG, "Performing hardware reset on GPIO21...");
  gpio_num_t reset_pin = GPIO_NUM_21;
  gpio_config_t io_conf = {.pin_bit_mask = (1ULL << reset_pin),
                           .mode = GPIO_MODE_OUTPUT,
                           .pull_up_en = GPIO_PULLUP_DISABLE,
                           .pull_down_en = GPIO_PULLDOWN_DISABLE,
                           .intr_type = GPIO_INTR_DISABLE};
  gpio_config(&io_conf);
  gpio_set_level(reset_pin, 0);
  vTaskDelay(pdMS_TO_TICKS(100));
  gpio_set_level(reset_pin, 1);
  vTaskDelay(pdMS_TO_TICKS(500));

  ESP_LOGI(TAG, "Calling va_dsp_init...");
  va_dsp_init(nullptr, nullptr, nullptr);
  ESP_LOGI(TAG, "va_dsp_init returned");

  if (this->mic_gain_ != 0.0f) {
    cx20921SetMicGain((int) this->mic_gain_);
  }

  this->stop_semaphore_ = xSemaphoreCreateBinary();
  this->ref_mutex_ = xSemaphoreCreateMutex();
}

void CXI2SMicrophone::start() {
  xSemaphoreTake(this->ref_mutex_, portMAX_DELAY);
  this->ref_count_++;
  if (this->task_running_) {
    ESP_LOGI(TAG, "Microphone task already running (ref_count=%d)", this->ref_count_);
    xSemaphoreGive(this->ref_mutex_);
    return;
  }

  // Fork parity: the reference firmware ran the mic task with an internal RAM
  // stack (xTaskCreatePinnedToCore allocates internally) pinned to core 1.
  // PSRAM task stacks on classic ESP32 were ruled out as the source of memory
  // corruption during concurrent announcement playback, so keep the stack in
  // internal RAM (32 KB; we have ~117 KB free after shrinking the I2S DMA
  // ring to 6x256).
  const uint32_t mic_stack_depth = 8192;  // words == 32 KB
  if (this->mic_stack_ == nullptr) {
    this->mic_stack_ =
        (StackType_t *) heap_caps_malloc(mic_stack_depth * sizeof(StackType_t),
                                         MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  }
  if (this->mic_stack_ == nullptr) {
    ESP_LOGE(TAG, "Failed to allocate PSRAM stack for microphone task!");
    this->state_ = microphone::STATE_STOPPED;
    xSemaphoreGive(this->ref_mutex_);
    return;
  }

  // Drain any stale exit token left by a previous (timed-out) stop so the
  // next stop() doesn't return immediately.
  xSemaphoreTake(this->stop_semaphore_, 0);

  this->task_running_ = true;
  this->state_ = microphone::STATE_RUNNING;

  this->mic_task_handle_ = xTaskCreateStaticPinnedToCore(mic_task, "cx_mic_task", mic_stack_depth, this, 5,
                                                         this->mic_stack_, &this->mic_tcb_, 1);

  if (this->mic_task_handle_ == nullptr) {
    ESP_LOGE(TAG, "Failed to create microphone task!");
    this->state_ = microphone::STATE_STOPPED;
    this->task_running_ = false;
    xSemaphoreGive(this->ref_mutex_);
    return;
  }

  ESP_LOGI(TAG, "Microphone task started on core 1");
  xSemaphoreGive(this->ref_mutex_);
}

void CXI2SMicrophone::stop() {
  // The whole stop sequence runs under the ref mutex: start()/stop() can be
  // called from different tasks (main loop vs. the mww inference task), and
  // only the mutex guarantees the static TCB/stack is never reused while the
  // old task still exists.
  xSemaphoreTake(this->ref_mutex_, portMAX_DELAY);
  if (this->ref_count_ > 0) {
    this->ref_count_--;
  }
  if (this->ref_count_ > 0) {
    ESP_LOGI(TAG, "Microphone still in use (ref_count=%d), not stopping", this->ref_count_);
    xSemaphoreGive(this->ref_mutex_);
    return;
  }

  if (!this->task_running_) {
    this->state_ = microphone::STATE_STOPPED;
    xSemaphoreGive(this->ref_mutex_);
    return;
  }

  ESP_LOGI(TAG, "Stopping microphone task...");

  this->task_running_ = false;

  if (this->stop_semaphore_ != nullptr) {
    if (xSemaphoreTake(this->stop_semaphore_, pdMS_TO_TICKS(200)) != pdTRUE) {
      ESP_LOGW(TAG, "Timeout waiting for task to stop");
    }
  }

  if (this->mic_task_handle_ != nullptr) {
    // Delete the task from this side. Unlike self-deletion (which defers TCB
    // cleanup to the idle task), deleting another task is synchronous: the
    // TCB is removed from all scheduler lists before vTaskDelete returns, so
    // the static TCB/stack can be reused safely by the next start().
    vTaskDelete(this->mic_task_handle_);
    this->mic_task_handle_ = nullptr;
  }

  this->state_ = microphone::STATE_STOPPED;
  ESP_LOGI(TAG, "Microphone stopped");
  xSemaphoreGive(this->ref_mutex_);
}

void CXI2SMicrophone::flush_buffers() {
  ESP_LOGD(TAG, "Flushing I2S DMA buffers...");

  i2s_zero_dma_buffer(I2S_NUM_1);

  uint8_t discard[640];
  size_t bytes_read;

  for (int i = 0; i < 10; i++) {
    i2s_read(I2S_NUM_1, discard, sizeof(discard), &bytes_read, pdMS_TO_TICKS(10));
  }

  ESP_LOGD(TAG, "Buffer flush complete");
}

void CXI2SMicrophone::mic_task(void *arg) {
  CXI2SMicrophone *self = static_cast<CXI2SMicrophone *>(arg);
  // Flush buffers здесь, а не в start()
  self->flush_buffers();

  ESP_LOGI(TAG, "Mic task running on core %d", xPortGetCoreID());

  uint32_t bytes_acc = 0;
  int64_t last_log = esp_timer_get_time();

  while (self->task_running_) {
    bytes_acc += self->read_loop();
    vTaskDelay(pdMS_TO_TICKS(1));
    int64_t now = esp_timer_get_time();
    if (now - last_log >= 1000000) {
      uint32_t rate = (uint32_t) ((uint64_t) bytes_acc * 1000000 / (uint64_t) (now - last_log));
      ESP_LOGI(TAG,
               "[mic_rate] %u bytes/s  (%u%% realtime) | free: internal %u KB (largest %u KB), psram %u KB",
               rate, rate * 100 / 32000,
               (unsigned) (heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
               (unsigned) (heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024),
               (unsigned) (heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
      bytes_acc = 0;
      last_log = now;
    }
  }

  ESP_LOGI(TAG, "Mic task exiting");

  if (self->stop_semaphore_ != nullptr) {
    xSemaphoreGive(self->stop_semaphore_);
  }

  // Park until stop() deletes this task. Self-deletion (vTaskDelete(nullptr))
  // defers TCB cleanup to the idle task; a quick restart could then reuse
  // this static TCB/stack before the cleanup happens and corrupt the
  // scheduler. Parking here lets stop() delete the task synchronously.
  vTaskSuspend(nullptr);
}

size_t CXI2SMicrophone::read_loop() {
  uint8_t stereo_buffer[640];
  size_t bytes_read = 0;

  esp_err_t err = i2s_read(I2S_NUM_1, stereo_buffer, sizeof(stereo_buffer), &bytes_read, pdMS_TO_TICKS(10));

  if (err != ESP_OK || bytes_read == 0) {
    return 0;
  }

  size_t samples = bytes_read / 4;
  // Reuse the member buffer across calls: allocating a fresh vector every
  // ~10 ms fragments the internal heap.
  this->mono_buffer_.clear();
  this->mono_buffer_.resize(samples * 2);

  for (size_t i = 0; i < samples; i++) {
    this->mono_buffer_[i * 2] = stereo_buffer[i * 4 + 0];
    this->mono_buffer_[i * 2 + 1] = stereo_buffer[i * 4 + 1];
  }

  if (!this->mono_buffer_.empty()) {
    this->data_callbacks_.call(this->mono_buffer_);
  }
  return this->mono_buffer_.size();
}

bool CXI2SMicrophone::set_mic_gain(float mic_gain) {
  this->mic_gain_ = clamp<float>(mic_gain, 0.0f, 30.0f);
  if (this->state_ == microphone::STATE_RUNNING) {
    cx20921SetMicGain((int) this->mic_gain_);
  }
  return true;
}

void CXI2SMicrophone::publish_data(const std::vector<uint8_t> &data) { this->data_callbacks_.call(data); }

void CXI2SSpeaker::setup() {}
void CXI2SSpeaker::start() { this->state_ = speaker::STATE_RUNNING; }
void CXI2SSpeaker::stop() { this->state_ = speaker::STATE_STOPPED; }
void CXI2SSpeaker::loop() {}
size_t CXI2SSpeaker::play(const uint8_t *data, size_t length) {
  // One-shot diagnostics: log the stream format and the first bytes that
  // actually reach I2S0 (debugging HF noise on playback).
  static bool logged = false;
  if (!logged) {
    const auto &info = this->get_audio_stream_info();
    char hex[49];
    size_t n = std::min(length, (size_t) 16);
    for (size_t i = 0; i < n; i++) {
      snprintf(hex + i * 3, 4, "%02X ", data[i]);
    }
    ESP_LOGI(TAG, "[spk] first write: %u bytes, stream %u Hz/%u ch/%u bps, data: %s", (unsigned) length,
             (unsigned) info.get_sample_rate(), (unsigned) info.get_channels(),
             (unsigned) info.get_bits_per_sample(), hex);
    logged = true;
  }
  size_t written = 0;
  i2s_write(I2S_NUM_0, data, length, &written, pdMS_TO_TICKS(10));

  if (written > 0) {
    // Report the frames handed to the DMA/DAC. Downstream components (mixer
    // source speakers, resamplers, media player progress) rely on this
    // callback to account for pending playback frames; without it the
    // announcement pipeline never reaches the stopped state.
    const uint32_t frames = this->audio_stream_info_.bytes_to_frames(written);
    const int64_t completion_ts =
        esp_timer_get_time() + (int64_t) this->audio_stream_info_.frames_to_microseconds(frames);
    this->audio_output_callback_(frames, completion_ts);
  }
  return written;
}
bool CXI2SSpeaker::has_buffered_data() const { return false; }

}  // namespace esphome::cx_i2s
