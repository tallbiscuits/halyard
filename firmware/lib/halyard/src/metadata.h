// SPDX-License-Identifier: GPL-3.0-or-later
// Slicer metadata, pulled out of a G-code file line by line as it streams onto Halyard's card.
// Marlin cannot send a file back, so this is the only chance to read it. Covers PrusaSlicer,
// OrcaSlicer (and other Slic3r descendants) and Cura. Field names follow Moonraker's
// server.files.metadata, which Mainsail reads. Portable C++, no Arduino.
#pragma once
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace halyard {

struct Thumbnail {
  int width = 0, height = 0;
  uint32_t size = 0;          // decoded PNG bytes
  std::string relative_path;  // ".thumbs/<stem>-<w>x<h>.png", relative to the library root
};

// A value below zero means "not found".
struct Metadata {
  std::string slicer, slicer_version;
  double estimated_time = -1;         // seconds
  double filament_total = -1;         // mm
  double filament_weight_total = -1;  // g
  double layer_height = -1, first_layer_height = -1, object_height = -1;
  double first_layer_extr_temp = -1, first_layer_bed_temp = -1;
  int layer_count = -1;
  std::vector<Thumbnail> thumbnails;

  // "key=value" lines, one field each; read back by parse(). Unknown keys are ignored.
  std::string serialize() const;
  static Metadata parse(const std::string& text);
};

// "1d 2h 16m 46s" -> seconds, or -1.
double parse_duration(const std::string& s);

// Feed every line of the file, in order. Thumbnails are decoded as they pass: open_thumb is
// asked for a FILE* to write the PNG to (nullptr skips it) and the file is closed at the end
// of the block. A block that turns out broken is left out of the result, but its file is not
// deleted: the caller removes what it opened and the result doesn't list. Nothing larger
// than one line is held in memory.
class MetadataParser {
 public:
  using OpenThumb = std::function<FILE*(int w, int h, std::string& relative_path)>;
  explicit MetadataParser(OpenThumb open_thumb = nullptr) : open_thumb_(std::move(open_thumb)) {}
  ~MetadataParser();

  void line(const std::string& l);
  Metadata finish();  // resolves fallbacks; call once after the last line

 private:
  void comment(const std::string& c);
  void command(const std::string& l);
  void thumb_data(const std::string& c);
  void end_thumb(bool keep);

  OpenThumb open_thumb_;
  Metadata m_;
  // Temperatures from the first M104/M109 and M140/M190, used when no comment names them
  // (Cura). Slic3r-family slicers write theirs at the end of the file, so they win later.
  double cmd_extr_ = -1, cmd_bed_ = -1;
  // The thumbnail being decoded.
  FILE* thumb_ = nullptr;
  Thumbnail cur_;
  uint32_t bits_ = 0;
  int nbits_ = 0;
  bool in_thumb_ = false, thumb_bad_ = false;
};

}  // namespace halyard
