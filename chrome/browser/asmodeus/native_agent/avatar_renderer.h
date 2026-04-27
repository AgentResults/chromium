// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_ASMODEUS_NATIVE_AGENT_AVATAR_RENDERER_H_
#define CHROME_BROWSER_ASMODEUS_NATIVE_AGENT_AVATAR_RENDERER_H_

#include <string>

#include "third_party/skia/include/core/SkBitmap.h"
#include "third_party/skia/include/core/SkCanvas.h"
#include "third_party/skia/include/core/SkColor.h"

namespace asmodeus {

// Renders a cartoon avatar face with lip sync. Each agent gets a
// deterministic appearance derived from their name hash.
class AvatarRenderer {
 public:
  struct Appearance {
    SkColor skin;
    SkColor hair;
    SkColor eye_color;
    SkColor lip_color;
    SkColor background;
    int hair_style;  // 0-3
  };

  AvatarRenderer(const std::string& name, int width, int height);
  ~AvatarRenderer();

  // Render a frame. jaw_open: 0.0 (closed) to 1.0 (wide open).
  SkBitmap Render(float jaw_open);

  const Appearance& appearance() const { return appearance_; }
  const std::string& name() const { return name_; }
  int width() const { return width_; }
  int height() const { return height_; }

 private:
  static Appearance GenerateAppearance(const std::string& name);

  void DrawBackground(SkCanvas* canvas);
  void DrawHead(SkCanvas* canvas, float cx, float cy, float rx, float ry);
  void DrawHair(SkCanvas* canvas, float cx, float cy, float rx, float ry);
  void DrawEyes(SkCanvas* canvas, float cx, float cy, float head_rx);
  void DrawEyebrows(SkCanvas* canvas, float cx, float cy, float head_rx);
  void DrawNose(SkCanvas* canvas, float cx, float cy);
  void DrawMouth(SkCanvas* canvas, float cx, float cy, float jaw_open);
  void DrawNameLabel(SkCanvas* canvas);

  std::string name_;
  std::string display_name_;
  int width_;
  int height_;
  Appearance appearance_;
};

}  // namespace asmodeus

#endif  // CHROME_BROWSER_ASMODEUS_NATIVE_AGENT_AVATAR_RENDERER_H_
