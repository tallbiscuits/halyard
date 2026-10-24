// SPDX-License-Identifier: GPL-3.0-or-later
// Writes a firmware.bin to the other app slot, for the update page (/halyard/update) and the
// Update Manager (updates.cpp). It checks on the way that the file is an ESP32-S3 app image,
// and at the end the image itself (esp_ota_end: chip, segments, SHA-256), before making it the
// boot image. The new image boots on trial (main.cpp).
#pragma once
#include <esp_ota_ops.h>

#include <cstddef>
#include <cstdint>
#include <string>

namespace ota {

class Writer {
 public:
  ~Writer();  // abandons an image that wasn't finished
  // `size` is the image's, if known (0: not known). False with error() and status() set.
  bool begin(size_t size);
  bool write(const uint8_t* data, size_t len);  // false: stop, error() says why
  bool finish();  // checks the image and boots it at the next restart
  const std::string& error() const { return error_; }
  int status() const { return status_; }  // an HTTP status for the error
  const char* partition() const { return part_ ? part_->label : ""; }

 private:
  bool fail(int status, const char* why);

  const esp_partition_t* part_ = nullptr;
  esp_ota_handle_t handle_ = 0;
  bool open_ = false;
  uint8_t head_[36];
  size_t have_ = 0;
  std::string error_;
  int status_ = 0;
};

}  // namespace ota
