// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/asmodeus/native_agent/avatar_renderer.h"

#include <algorithm>
#include <cmath>

#include "base/hash/hash.h"
#include "third_party/skia/include/core/SkCanvas.h"
#include "third_party/skia/include/core/SkFont.h"
#include "third_party/skia/include/core/SkPathBuilder.h"
#include "third_party/skia/include/core/SkRRect.h"

namespace asmodeus {

namespace {

constexpr SkColor kSkinPalette[] = {
    0xFFF5D0A9, 0xFFE8C39E, 0xFFD4A574,
    0xFFC68642, 0xFF8D5524, 0xFF6B3A2A,
};

constexpr SkColor kHairPalette[] = {
    0xFF4A2F1B, 0xFF2C1810, 0xFF8B6914, 0xFFD4A017,
    0xFF1A1A2E, 0xFF8B0000, 0xFF2F4F4F, 0xFF696969,
};

constexpr SkColor kEyePalette[] = {
    0xFF2E86C1, 0xFF27AE60, 0xFF8B4513,
    0xFF1A1A2E, 0xFF6B8E23, 0xFF4A235A,
};

constexpr SkColor kBgPalette[] = {
    0xFF1A1A2E, 0xFF16213E, 0xFF0F3460,
    0xFF1B2631, 0xFF0E1117, 0xFF1C2833,
};

}  // namespace

AvatarRenderer::AvatarRenderer(const std::string& name, int width, int height)
    : name_(name), width_(width), height_(height) {
  display_name_ = name;
  if (!display_name_.empty()) {
    display_name_[0] = toupper(display_name_[0]);
  }
  appearance_ = GenerateAppearance(name);
}

AvatarRenderer::~AvatarRenderer() = default;

AvatarRenderer::Appearance AvatarRenderer::GenerateAppearance(
    const std::string& name) {
  uint32_t h = base::PersistentHash(name);
  Appearance a;
  a.skin = kSkinPalette[h % std::size(kSkinPalette)];
  a.hair = kHairPalette[(h >> 4) % std::size(kHairPalette)];
  a.eye_color = kEyePalette[(h >> 8) % std::size(kEyePalette)];
  {
    int r = static_cast<int>(SkColorGetR(a.skin)) + 40;
    int g = static_cast<int>(SkColorGetG(a.skin)) - 30;
    int b = static_cast<int>(SkColorGetB(a.skin)) - 20;
    a.lip_color = SkColorSetARGB(255,
        std::clamp(r, 0, 255), std::clamp(g, 0, 255), std::clamp(b, 0, 255));
  }
  a.background = kBgPalette[(h >> 12) % std::size(kBgPalette)];
  a.hair_style = (h >> 16) % 4;
  return a;
}

SkBitmap AvatarRenderer::Render(float jaw_open) {
  jaw_open = std::clamp(jaw_open, 0.0f, 1.0f);

  SkBitmap bitmap;
  bitmap.allocN32Pixels(width_, height_);
  SkCanvas canvas(bitmap);

  float cx = width_ / 2.0f;
  float cy = height_ * 0.42f;
  float head_rx = width_ * 0.35f;
  float head_ry = height_ * 0.38f;

  DrawBackground(&canvas);
  DrawHead(&canvas, cx, cy, head_rx, head_ry);
  DrawHair(&canvas, cx, cy, head_rx, head_ry);
  DrawEyes(&canvas, cx, cy, head_rx);
  DrawEyebrows(&canvas, cx, cy, head_rx);
  DrawNose(&canvas, cx, cy);
  DrawMouth(&canvas, cx, cy, jaw_open);
  DrawNameLabel(&canvas);

  return bitmap;
}

void AvatarRenderer::DrawBackground(SkCanvas* canvas) {
  canvas->clear(appearance_.background);
}

void AvatarRenderer::DrawHead(SkCanvas* canvas, float cx, float cy,
                               float rx, float ry) {
  SkPaint paint;
  paint.setColor(appearance_.skin);
  paint.setAntiAlias(true);
  canvas->drawOval(SkRect::MakeXYWH(cx - rx, cy - ry, rx * 2, ry * 2), paint);
}

void AvatarRenderer::DrawHair(SkCanvas* canvas, float cx, float cy,
                               float rx, float ry) {
  SkPaint paint;
  paint.setColor(appearance_.hair);
  paint.setAntiAlias(true);

  // Simple hair: an oval on top of the head
  float hair_ry = ry * 0.5f;
  float hair_rx = rx * 1.05f;
  float hair_cy = cy - ry * 0.55f;
  canvas->drawOval(
      SkRect::MakeXYWH(cx - hair_rx, hair_cy - hair_ry,
                        hair_rx * 2, hair_ry * 2),
      paint);
}

void AvatarRenderer::DrawEyes(SkCanvas* canvas, float cx, float cy,
                               float head_rx) {
  float eye_y = cy - head_rx * 0.05f;
  float eye_spacing = head_rx * 0.4f;
  float eye_rx = head_rx * 0.15f;
  float eye_ry = eye_rx * 1.2f;

  for (int side = -1; side <= 1; side += 2) {
    float ex = cx + side * eye_spacing;

    // White
    SkPaint white;
    white.setColor(SK_ColorWHITE);
    white.setAntiAlias(true);
    canvas->drawOval(
        SkRect::MakeXYWH(ex - eye_rx, eye_y - eye_ry, eye_rx * 2, eye_ry * 2),
        white);

    // Iris
    float iris_r = eye_rx * 0.6f;
    SkPaint iris;
    iris.setColor(appearance_.eye_color);
    iris.setAntiAlias(true);
    canvas->drawCircle(ex, eye_y, iris_r, iris);

    // Pupil
    float pupil_r = iris_r * 0.5f;
    SkPaint pupil;
    pupil.setColor(SK_ColorBLACK);
    pupil.setAntiAlias(true);
    canvas->drawCircle(ex, eye_y, pupil_r, pupil);

    // Highlight
    float hl_r = pupil_r * 0.4f;
    SkPaint highlight;
    highlight.setColor(SkColorSetARGB(200, 255, 255, 255));
    highlight.setAntiAlias(true);
    canvas->drawCircle(ex + iris_r * 0.3f, eye_y - iris_r * 0.3f, hl_r,
                        highlight);
  }
}

void AvatarRenderer::DrawEyebrows(SkCanvas* canvas, float cx, float cy,
                                    float head_rx) {
  float brow_y = cy - head_rx * 0.25f;
  float eye_spacing = head_rx * 0.4f;
  float brow_w = head_rx * 0.18f;

  SkPaint paint;
  {
    int r = static_cast<int>(SkColorGetR(appearance_.hair)) - 30;
    int g = static_cast<int>(SkColorGetG(appearance_.hair)) - 30;
    int b = static_cast<int>(SkColorGetB(appearance_.hair)) - 30;
    paint.setColor(SkColorSetARGB(255,
        std::clamp(r, 0, 255), std::clamp(g, 0, 255), std::clamp(b, 0, 255)));
  }
  paint.setAntiAlias(true);
  paint.setStyle(SkPaint::kStroke_Style);
  paint.setStrokeWidth(head_rx * 0.05f);
  paint.setStrokeCap(SkPaint::kRound_Cap);

  for (int side = -1; side <= 1; side += 2) {
    float bx = cx + side * eye_spacing;
    canvas->drawLine(bx - brow_w, brow_y, bx + brow_w, brow_y, paint);
  }
}

void AvatarRenderer::DrawNose(SkCanvas* canvas, float cx, float cy) {
  float nose_y = cy + height_ * 0.06f;
  float nose_size = width_ * 0.02f;

  SkPaint paint;
  {
    int r = static_cast<int>(SkColorGetR(appearance_.skin)) - 25;
    int g = static_cast<int>(SkColorGetG(appearance_.skin)) - 15;
    int b = static_cast<int>(SkColorGetB(appearance_.skin)) - 10;
    paint.setColor(SkColorSetARGB(255,
        std::clamp(r, 0, 255), std::clamp(g, 0, 255), std::clamp(b, 0, 255)));
  }
  paint.setAntiAlias(true);

  // Simple triangle nose using drawPath with SkPathBuilder
  SkPathBuilder builder;
  builder.moveTo(cx, nose_y - nose_size);
  builder.lineTo(cx - nose_size, nose_y + nose_size);
  builder.lineTo(cx + nose_size, nose_y + nose_size);
  builder.close();
  canvas->drawPath(builder.detach(), paint);
}

void AvatarRenderer::DrawMouth(SkCanvas* canvas, float cx, float cy,
                                float jaw_open) {
  float mouth_y = cy + height_ * 0.14f;
  float mouth_w = width_ * 0.08f + jaw_open * width_ * 0.04f;
  float mouth_h = height_ * 0.01f + jaw_open * height_ * 0.08f;

  SkPaint paint;
  paint.setColor(appearance_.lip_color);
  paint.setAntiAlias(true);

  if (jaw_open < 0.1f) {
    // Closed mouth — just a line
    paint.setStyle(SkPaint::kStroke_Style);
    paint.setStrokeWidth(2.5f);
    paint.setStrokeCap(SkPaint::kRound_Cap);
    canvas->drawLine(cx - mouth_w, mouth_y, cx + mouth_w, mouth_y, paint);
  } else {
    // Open mouth — ellipse
    paint.setStyle(SkPaint::kFill_Style);
    canvas->drawOval(
        SkRect::MakeXYWH(cx - mouth_w, mouth_y - mouth_h / 2,
                          mouth_w * 2, mouth_h),
        paint);

    // Dark interior
    if (jaw_open > 0.3f) {
      SkPaint dark;
      dark.setColor(SkColorSetARGB(200, 60, 20, 20));
      dark.setAntiAlias(true);
      float inner_w = mouth_w * 0.7f;
      float inner_h = mouth_h * 0.6f;
      canvas->drawOval(
          SkRect::MakeXYWH(cx - inner_w, mouth_y - inner_h / 2,
                            inner_w * 2, inner_h),
          dark);
    }

    // Teeth
    if (jaw_open > 0.4f) {
      SkPaint teeth;
      teeth.setColor(SkColorSetARGB(230, 255, 255, 255));
      teeth.setAntiAlias(true);
      float teeth_w = mouth_w * 0.5f;
      float teeth_h = mouth_h * 0.2f;
      canvas->drawRect(
          SkRect::MakeXYWH(cx - teeth_w, mouth_y - mouth_h * 0.3f,
                            teeth_w * 2, teeth_h),
          teeth);
    }
  }
}

void AvatarRenderer::DrawNameLabel(SkCanvas* canvas) {
  float label_y = height_ * 0.88f;

  SkFont font;
  font.setSize(height_ * 0.06f);

  SkRect text_bounds;
  font.measureText(display_name_.c_str(), display_name_.size(),
                   SkTextEncoding::kUTF8, &text_bounds);
  float text_x = (width_ - text_bounds.width()) / 2.0f;

  SkPaint paint;
  paint.setColor(SkColorSetARGB(180, 200, 200, 200));
  paint.setAntiAlias(true);

  canvas->drawString(display_name_.c_str(), text_x, label_y, font, paint);
}

}  // namespace asmodeus
