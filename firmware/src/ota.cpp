// SPDX-License-Identifier: GPL-3.0-or-later
#include "ota.h"

#include <algorithm>
#include <cstring>

namespace ota {

Writer::~Writer() {
  if (open_) esp_ota_abort(handle_);
}

bool Writer::fail(int status, const char* why) {
  if (open_) esp_ota_abort(handle_);
  open_ = false;
  if (error_.empty()) {
    error_ = why;
    status_ = status;
  }
  return false;
}

bool Writer::begin(size_t size) {
  part_ = esp_ota_get_next_update_partition(nullptr);
  if (!part_) return fail(500, "no OTA partition");
  if (size > part_->size) return fail(413, "not a firmware image of a size that fits");
  if (esp_ota_begin(part_, OTA_WITH_SEQUENTIAL_WRITES, &handle_) != ESP_OK)
    return fail(500, "cannot start the update");
  open_ = true;
  return true;
}

bool Writer::write(const uint8_t* d, size_t n) {
  if (!open_) return false;
  // The image header and the app description after the first segment header: an ESP32-S3
  // app, not the bootloader a merged (USB flash) image starts with, whose checks would pass.
  if (have_ < sizeof head_) {
    size_t take = std::min(n, sizeof head_ - have_);
    memcpy(head_ + have_, d, take);
    have_ += take;
    if (have_ == sizeof head_) {
      uint32_t desc = head_[32] | head_[33] << 8 | head_[34] << 16 | (uint32_t)head_[35] << 24;
      if (head_[0] != 0xE9) return fail(400, "not an ESP32 firmware image");
      if ((head_[12] | head_[13] << 8) != 9) return fail(400, "an image for another chip, not the ESP32-S3");
      if (desc != 0xABCD5432) return fail(400, "not an app image (use firmware.bin, not a USB flash image)");
    }
  }
  if (esp_ota_write(handle_, d, n) != ESP_OK) return fail(500, "flash write failed");
  return true;
}

bool Writer::finish() {
  if (!open_) return false;
  if (have_ < sizeof head_) return fail(400, "not an ESP32 firmware image");
  open_ = false;  // esp_ota_end frees the handle, whatever it returns
  esp_err_t err = esp_ota_end(handle_);
  if (err != ESP_OK)
    return fail(400, err == ESP_ERR_OTA_VALIDATE_FAILED ? "the image is invalid" : "cannot finish the update");
  if (esp_ota_set_boot_partition(part_) != ESP_OK) return fail(500, "cannot switch to the new image");
  return true;
}

}  // namespace ota
