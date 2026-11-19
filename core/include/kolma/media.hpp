// kolma -- perceptual media shrinking. Archiving a video cannot shrink it (it is
// already entropy-coded); this re-encodes it to a smaller file that stays directly
// playable, trading a little quality for a lot of size. Uses the system ffmpeg.
#pragma once

#include <string>

#include "kolma/engine.hpp"

namespace kolma {

// Locates an ffmpeg binary on PATH ("" when absent). The feature is optional at
// runtime: without ffmpeg the rest of kolma still works.
std::string find_ffmpeg();
bool media_available();

struct MediaOptions {
  std::string output;             // empty = <base>-shrunk.mp4 (video) / .jpg (image)
  std::string preset = "balanced";  // quality | balanced | smallest
  bool force = false;
};

// Re-encodes a video or image to a smaller, still-playable/viewable file and
// reports before/after sizes. Returns an error (not a crash) when ffmpeg is
// missing or the input is not media.
Result shrink_media(const std::string& input, const MediaOptions& opts,
                    const ProgressFn& on_progress = nullptr);

}  // namespace kolma
