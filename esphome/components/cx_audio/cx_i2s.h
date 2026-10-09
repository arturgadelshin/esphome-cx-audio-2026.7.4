#pragma once

#include "esphome/core/component.h"
#include "esphome/components/microphone/microphone.h"
#include "esphome/components/speaker/speaker.h"
#include "cx_audio.h"
#include <vector>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

namespace esphome::cx_i2s {

class CXI2SMicrophone : public microphone::Microphone, public Component {
 public:
  void setup() override;
  void start() override;
  void stop() override;
  void loop() override {}
  void set_cx_audio(cx_audio::CXAudio *parent) { this->parent_ = parent; }
  bool is_running() const { return this->state_ == microphone::STATE_RUNNING; }
  void set_simulate_stall(bool simulate) { this->simulate_stall_ = simulate; }
  void publish_data(const std::vector<uint8_t> &data);
  bool set_mic_gain(float mic_gain);
  float get_mic_gain_value() const { return mic_gain_; }
  float mic_gain() { return this->mic_gain_; }

 protected:
  cx_audio::CXAudio *parent_;
  float mic_gain_{24.0f};

  TaskHandle_t mic_task_handle_{nullptr};
  StaticTask_t mic_tcb_;
  StackType_t *mic_stack_{nullptr};
  SemaphoreHandle_t stop_semaphore_{nullptr};
  SemaphoreHandle_t ref_mutex_{nullptr};
  std::vector<uint8_t> mono_buffer_;
  volatile bool task_running_{false};
  int ref_count_{0};

  static void mic_task(void *arg);
  size_t read_loop();
  void flush_buffers();
  void recover_dsp_();

  static const uint32_t MIC_STALL_TIMEOUT_MS = 2000;
  static const uint32_t MIC_STALL_MAX_RECOVERIES = 3;

  volatile bool simulate_stall_{false};
  bool stall_active_{false};
  uint32_t stall_since_ms_{0};
  uint32_t stall_recoveries_{0};
};

class CXI2SSpeaker : public speaker::Speaker, public Component {
 public:
  void setup() override;
  void start() override;
  void stop() override;
  void loop() override;
  size_t play(const uint8_t *data, size_t length) override;
  bool has_buffered_data() const override;
  void set_cx_audio(cx_audio::CXAudio *parent) { this->parent_ = parent; }

 protected:
  cx_audio::CXAudio *parent_;
  std::vector<uint8_t> partial_buffer_;

  void summarize_session_();

  // Stutter diagnostics: the I2S0 DMA ring holds ~35 ms of audio at 44.1 kHz
  // stereo, so any gap between consecutive play() calls longer than the ring
  // depth means the DAC ran dry and the playback audibly stuttered.
  // Gaps > 1 s are stream boundaries (mixer idles between announcements with
  // the DAC muted), not stutters: they start a new session instead.
  static const uint32_t STUTTER_GAP_MS = 40;
  static const uint32_t STREAM_BOUNDARY_MS = 1000;
  int64_t last_play_us_{0};
  int64_t last_underrun_log_us_{0};
  int64_t session_start_us_{0};
  size_t session_bytes_{0};
  uint32_t underruns_{0};
  uint32_t max_gap_ms_{0};
  bool session_summarized_{true};
};

}  // namespace esphome::cx_i2s
