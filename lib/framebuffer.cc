// -*- mode: c++; c-basic-offset: 2; indent-tabs-mode: nil; -*-
// Copyright (C) 2013 Henner Zeller <h.zeller@acm.org>
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation version 2.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://gnu.org/licenses/gpl-2.0.txt>

// The framebuffer is the workhorse: it represents the frame in some internal
// format that is friendly to be dumped to the matrix quickly. Provides methods
// to manipulate the content.

#include "framebuffer-internal.h"

#include <stdio.h>
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

#include "gpio.h"

namespace rgb_matrix {
namespace internal {
enum {
  kBitPlanes = 11  // maximum usable bitplanes.
};

// Lower values create a higher framerate, but display will be a
// bit dimmer. Good values are between 100 and 200.
static const long kBaseTimeNanos = 130;

// We need one global instance of a timing correct pulser. There are different
// implementations depending on the context.
static PinPulser *sOutputEnablePulser = NULL;

// The Adafruit HAT only supports one chain.
#if defined(ADAFRUIT_RGBMATRIX_HAT) || defined(ADAFRUIT_RGBMATRIX_HAT_PWM)
#  define ONLY_SINGLE_CHAIN 1
#endif

#ifdef RGB_SWAP_GREEN_BLUE
#  define PANEL_SWAP_G_B_ 1
#else
#  define PANEL_SWAP_G_B_ 0
#endif

#ifdef ONLY_SINGLE_SUB_PANEL
#  define SUB_PANELS_ 1
#else
#  define SUB_PANELS_ 2
#endif

Framebuffer::Framebuffer(int rows, int columns, int parallel)
  : rows_(rows),
    parallel_(parallel),
    height_(rows * parallel),
    columns_(columns),
    pwm_bits_(kBitPlanes), do_luminance_correct_(true), brightness_(100),
    double_rows_(rows / SUB_PANELS_), row_mask_(double_rows_ - 1) {
  bitplane_buffer_0 = new IoBits0 [double_rows_ * columns_ * kBitPlanes];
#ifdef CM_5_CHAIN_SUPPORT
  bitplane_buffer_1 = new IoBits1 [double_rows_ * columns_ * kBitPlanes];
#endif
  Clear();
  assert(rows_ == 8 || rows_ == 16 || rows_ == 32 || rows_ == 64);
#ifdef CM_5_CHAIN_SUPPORT
  assert(parallel >= 1 && parallel <= 5);
#else
  assert(parallel >= 1 && parallel <= 3);
#endif

#ifdef ONLY_SINGLE_CHAIN
  if (parallel > 1) {
    fprintf(stderr, "ONLY_SINGLE_CHAIN is defined, but parallel > 1 given\n");
    assert(parallel == 1);
  }
#endif
}

Framebuffer::~Framebuffer() {
  delete [] bitplane_buffer_0;
#ifdef CM_5_CHAIN_SUPPORT
  delete [] bitplane_buffer_1;
#endif
}

/* static */ void Framebuffer::InitGPIO(GPIO *io, int rows, int parallel) {
  if (sOutputEnablePulser != NULL)
    return;  // already initialized.

  // Tell GPIO about all bits we intend to use.
  IoBits0 b0;
  b0.raw = 0;

#ifdef CM_5_CHAIN_SUPPORT
  IoBits1 b1;
  b1.raw = 0;
#endif

#ifdef PI_REV1_RGB_PINOUT_
  b0.bits.output_enable_rev1 = b0.bits.output_enable_rev2 = 1;
  b0.bits.clock_rev1 = b0.bits.clock_rev2 = 1;
#endif

  b0.bits.output_enable = 1;
  b0.bits.clock = 1;
  b0.bits.strobe = 1;

  b0.bits.p0_r1 = b0.bits.p0_g1 = b0.bits.p0_b1 = 1;
  b0.bits.p0_r2 = b0.bits.p0_g2 = b0.bits.p0_b2 = 1;

#ifndef ONLY_SINGLE_CHAIN
  if (parallel >= 2) {
    b0.bits.p1_r1 = b0.bits.p1_g1 = b0.bits.p1_b1 = 1;
    b0.bits.p1_r2 = b0.bits.p1_g2 = b0.bits.p1_b2 = 1;
  }

  if (parallel >= 3) {
    b0.bits.p2_r1 = b0.bits.p2_g1 = b0.bits.p2_b1 = 1;
    b0.bits.p2_r2 = b0.bits.p2_g2 = b0.bits.p2_b2 = 1;
  }
#endif

#ifdef CM_5_CHAIN_SUPPORT
  if (parallel >= 2) {
    b1.bits.p3_r1 = b1.bits.p3_g1 = b1.bits.p3_b1 = 1;
    b1.bits.p3_r2 = b1.bits.p3_g2 = b1.bits.p3_b2 = 1;
  }

  if (parallel >= 3) {
    b1.bits.p4_r1 = b1.bits.p4_g1 = b1.bits.p4_b1 = 1;
    b1.bits.p4_r2 = b1.bits.p4_g2 = b1.bits.p4_b2 = 1;
  }
#endif


  const int double_rows = rows / 2;
  if (double_rows >= 32) b0.bits.e = 1;
  if (double_rows >= 16) b0.bits.d = 1;
  if (double_rows >=  8) b0.bits.c = 1;
  if (double_rows >=  4) b0.bits.b = 1;
  b0.bits.a = 1;

  // Initialize outputs, make sure that all of these are supported bits.
  uint32_t result = io->InitOutputs0(b0.raw);
  assert(result == b0.raw);

#ifdef CM_5_CHAIN_SUPPORT
  result = io->InitOutputs1(b1.raw);
  assert(result == b1.raw);
#endif


  // Now, set up the PinPulser for output enable.
  IoBits0 output_enable_bits0;
#ifdef PI_REV1_RGB_PINOUT_
  output_enable_bits0.bits.output_enable_rev1
    = output_enable_bits0.bits.output_enable_rev2 = 1;
#endif
  output_enable_bits0.bits.output_enable = 1;

  std::vector<int> bitplane_timings;
  for (int b = 0; b < kBitPlanes; ++b) {
    bitplane_timings.push_back(kBaseTimeNanos << b);
  }
  sOutputEnablePulser = PinPulser::Create(io, output_enable_bits0.raw,
                                          bitplane_timings);
}

bool Framebuffer::SetPWMBits(uint8_t value) {
  if (value < 1 || value > kBitPlanes)
    return false;
  pwm_bits_ = value;
  return true;
}

inline Framebuffer::IoBits0 *Framebuffer::ValueAt_0(int double_row,
                                                 int column, int bit) {
  return &bitplane_buffer_0[ double_row * (columns_ * kBitPlanes)
                            + bit * columns_
                            + column ];
}

#ifdef CM_5_CHAIN_SUPPORT
inline Framebuffer::IoBits1 *Framebuffer::ValueAt_1(int double_row,
                                                 int column, int bit) {
  return &bitplane_buffer_1[ double_row * (columns_ * kBitPlanes)
                            + bit * columns_
                            + column ];
}
#endif


// Do CIE1931 luminance correction and scale to output bitplanes
static uint16_t luminance_cie1931(uint8_t c, uint8_t brightness) {
  float out_factor = ((1 << kBitPlanes) - 1);
  float v = (float) c * brightness / 255.0;
  return out_factor * ((v <= 8) ? v / 902.3 : pow((v + 16) / 116.0, 3));
}

static uint16_t *CreateLuminanceCIE1931LookupTable() {
  uint16_t *result = new uint16_t[256 * 100];
  for (int i = 0; i < 256; ++i)
    for (int j = 0; j < 100; ++j)
      result[i * 100 + j] = luminance_cie1931(i, j + 1);

  return result;
}

inline uint16_t Framebuffer::MapColor(uint8_t c) {
#ifdef INVERSE_RGB_DISPLAY_COLORS
#  define COLOR_OUT_BITS(x) (x) ^ 0xffff
#else
#  define COLOR_OUT_BITS(x) (x)
#endif

  if (do_luminance_correct_) {
    static uint16_t *luminance_lookup = CreateLuminanceCIE1931LookupTable();
    return COLOR_OUT_BITS(luminance_lookup[c * 100 + (brightness_ - 1)]);
  } else {
    // simple scale down the color value
    c = c * brightness_ / 100;

    enum {shift = kBitPlanes - 8};  //constexpr; shift to be left aligned.
    return COLOR_OUT_BITS((shift > 0) ? (c << shift) : (c >> -shift));
  }

#undef COLOR_OUT_BITS
}

void Framebuffer::Clear() {
#ifdef INVERSE_RGB_DISPLAY_COLORS
  Fill(0, 0, 0);
#else
  memset(bitplane_buffer_0, 0,
         sizeof(*bitplane_buffer_0) * double_rows_ * columns_ * kBitPlanes);
 #ifdef CM_5_CHAIN_SUPPORT
  memset(bitplane_buffer_1, 0,
         sizeof(*bitplane_buffer_1) * double_rows_ * columns_ * kBitPlanes);
 #endif
#endif
}

void Framebuffer::Fill(uint8_t r, uint8_t g, uint8_t b) {
  const uint16_t red   = MapColor(r);
  const uint16_t green = MapColor(PANEL_SWAP_G_B_ ? b : g);
  const uint16_t blue  = MapColor(PANEL_SWAP_G_B_ ? g : b);

  for (int b = kBitPlanes - pwm_bits_; b < kBitPlanes; ++b) {
    uint16_t mask = 1 << b;
    IoBits0 plane_bits0;
#ifdef CM_5_CHAIN_SUPPORT
    IoBits1 plane_bits1;
#endif

    plane_bits0.raw = 0;
    plane_bits0.bits.p0_r1 = plane_bits0.bits.p0_r2 = (red & mask) == mask;
    plane_bits0.bits.p0_g1 = plane_bits0.bits.p0_g2 = (green & mask) == mask;
    plane_bits0.bits.p0_b1 = plane_bits0.bits.p0_b2 = (blue & mask) == mask;

#ifndef ONLY_SINGLE_CHAIN
    plane_bits0.bits.p1_r1 = plane_bits0.bits.p1_r2 =
      plane_bits0.bits.p2_r1 = plane_bits0.bits.p2_r2 = (red & mask) == mask;
    plane_bits0.bits.p1_g1 = plane_bits0.bits.p1_g2 =
      plane_bits0.bits.p2_g1 = plane_bits0.bits.p2_g2 = (green & mask) == mask;
    plane_bits0.bits.p1_b1 = plane_bits0.bits.p1_b2 =
      plane_bits0.bits.p2_b1 = plane_bits0.bits.p2_b2 = (blue & mask) == mask;
#endif

#ifdef CM_5_CHAIN_SUPPORT
    plane_bits1.raw = 0;
    plane_bits1.bits.p3_r1 = plane_bits1.bits.p3_r2 =
      plane_bits1.bits.p4_r1 = plane_bits1.bits.p4_r2 = (red & mask) == mask;
    plane_bits1.bits.p3_g1 = plane_bits1.bits.p3_g2 =
      plane_bits1.bits.p4_g1 = plane_bits1.bits.p4_g2 = (green & mask) == mask;
    plane_bits1.bits.p3_b1 = plane_bits1.bits.p3_b2 =
      plane_bits1.bits.p4_b1 = plane_bits1.bits.p4_b2 = (blue & mask) == mask;
#endif

    for (int row = 0; row < double_rows_; ++row) {
      IoBits0 *row_data0 = ValueAt_0(row, 0, b);
#ifdef CM_5_CHAIN_SUPPORT
      IoBits1 *row_data1 = ValueAt_1(row, 0, b);
#endif
      for (int col = 0; col < columns_; ++col) {
        (row_data0++)->raw = plane_bits0.raw;
#ifdef CM_5_CHAIN_SUPPORT
        (row_data1++)->raw = plane_bits1.raw;;
#endif
      }
    }
  }
}

void Framebuffer::SetPixel(int x, int y, uint8_t r, uint8_t g, uint8_t b) {
  if (x < 0 || x >= columns_ || y < 0 || y >= height_) return;

  const uint16_t red   = MapColor(r);
  const uint16_t green = MapColor(PANEL_SWAP_G_B_ ? b : g);
  const uint16_t blue  = MapColor(PANEL_SWAP_G_B_ ? g : b);

  const int min_bit_plane = kBitPlanes - pwm_bits_;
  IoBits0 *bits0 = ValueAt_0(y & row_mask_, x, min_bit_plane);
#ifdef CM_5_CHAIN_SUPPORT
  IoBits1 *bits1 = ValueAt_1(y & row_mask_, x, min_bit_plane);
#endif

  // Manually expand the three cases for better performance.
  // TODO(hzeller): This is a bit repetetive. Test if it pays off to just
  // pre-calc rgb mask and apply.
  if (y < rows_) {
    // Parallel chain #1
    if (y < double_rows_) {   // Upper sub-panel.
      for (int b = min_bit_plane; b < kBitPlanes; ++b) {
        const uint16_t mask = 1 << b;
        bits0->bits.p0_r1 = (red & mask) == mask;
        bits0->bits.p0_g1 = (green & mask) == mask;
        bits0->bits.p0_b1 = (blue & mask) == mask;
        bits0 += columns_;
      }
    } else {
      for (int b = min_bit_plane; b < kBitPlanes; ++b) {
        const uint16_t mask = 1 << b;
        bits0->bits.p0_r2 = (red & mask) == mask;
        bits0->bits.p0_g2 = (green & mask) == mask;
        bits0->bits.p0_b2 = (blue & mask) == mask;
        bits0 += columns_;
      }
    }
#ifndef ONLY_SINGLE_CHAIN
  } else if (y >= rows_ && y < (2*rows_)) {
    // Parallel chain #2
    if (y - rows_ < double_rows_) {   // Upper sub-panel.
      for (int b = min_bit_plane; b < kBitPlanes; ++b) {
        const uint16_t mask = 1 << b;
        bits0->bits.p1_r1 = (red & mask) == mask;
        bits0->bits.p1_g1 = (green & mask) == mask;
        bits0->bits.p1_b1 = (blue & mask) == mask;
        bits0 += columns_;
      }
    } else {
      for (int b = min_bit_plane; b < kBitPlanes; ++b) {
        const uint16_t mask = 1 << b;
        bits0->bits.p1_r2 = (red & mask) == mask;
        bits0->bits.p1_g2 = (green & mask) == mask;
        bits0->bits.p1_b2 = (blue & mask) == mask;
        bits0 += columns_;
      }
    }
  } else if (y >= (2*rows_) && y < (3*rows_)) {
    // Parallel chain #3
    if (y - 2*rows_ < double_rows_) {   // Upper sub-panel.
      for (int b = min_bit_plane; b < kBitPlanes; ++b) {
        const uint16_t mask = 1 << b;
        bits0->bits.p2_r1 = (red & mask) == mask;
        bits0->bits.p2_g1 = (green & mask) == mask;
        bits0->bits.p2_b1 = (blue & mask) == mask;
        bits0 += columns_;
      }
    } else {
      for (int b = min_bit_plane; b < kBitPlanes; ++b) {
        const uint16_t mask = 1 << b;
        bits0->bits.p2_r2 = (red & mask) == mask;
        bits0->bits.p2_g2 = (green & mask) == mask;
        bits0->bits.p2_b2 = (blue & mask) == mask;
        bits0 += columns_;
      }
    }
#endif

#ifdef CM_5_CHAIN_SUPPORT
  } else if (y >= (3*rows_) && y < (4*rows_)) {
    // Parallel chain #4
    if (y - 3*rows_ < double_rows_) {   // Upper sub-panel.
      for (int b = min_bit_plane; b < kBitPlanes; ++b) {
        const uint16_t mask = 1 << b;
        bits1->bits.p3_r1 = (red & mask) == mask;
        bits1->bits.p3_g1 = (green & mask) == mask;
        bits1->bits.p3_b1 = (blue & mask) == mask;
        bits1 += columns_;
      }
    } else {
      for (int b = min_bit_plane; b < kBitPlanes; ++b) {
        const uint16_t mask = 1 << b;
        bits1->bits.p3_r2 = (red & mask) == mask;
        bits1->bits.p3_g2 = (green & mask) == mask;
        bits1->bits.p3_b2 = (blue & mask) == mask;
        bits1 += columns_;
      }
    }
  } else if (y >= (4*rows_) && y < (5*rows_)) {
    // Parallel chain #5
    if (y - 4*rows_ < double_rows_) {   // Upper sub-panel.
      for (int b = min_bit_plane; b < kBitPlanes; ++b) {
        const uint16_t mask = 1 << b;
        bits1->bits.p4_r1 = (red & mask) == mask;
        bits1->bits.p4_g1 = (green & mask) == mask;
        bits1->bits.p4_b1 = (blue & mask) == mask;
        bits1 += columns_;
      }
    } else {
      for (int b = min_bit_plane; b < kBitPlanes; ++b) {
        const uint16_t mask = 1 << b;
        bits1->bits.p4_r2 = (red & mask) == mask;
        bits1->bits.p4_g2 = (green & mask) == mask;
        bits1->bits.p4_b2 = (blue & mask) == mask;
        bits1 += columns_;
      }
    }
#endif
  }
}

void Framebuffer::DumpToMatrix(GPIO *io) {
  IoBits0 color_clk_mask0;   // Mask of bits we need to set while clocking in.
#ifdef CM_5_CHAIN_SUPPORT
  IoBits1 color_clk_mask1;
#endif
  color_clk_mask0.bits.p0_r1
    = color_clk_mask0.bits.p0_g1
    = color_clk_mask0.bits.p0_b1
    = color_clk_mask0.bits.p0_r2
    = color_clk_mask0.bits.p0_g2
    = color_clk_mask0.bits.p0_b2 = 1;

#ifndef ONLY_SINGLE_CHAIN
  if (parallel_ >= 2) {
    color_clk_mask0.bits.p1_r1
      = color_clk_mask0.bits.p1_g1
      = color_clk_mask0.bits.p1_b1
      = color_clk_mask0.bits.p1_r2
      = color_clk_mask0.bits.p1_g2
      = color_clk_mask0.bits.p1_b2 = 1;
  }

  if (parallel_ >= 3) {
    color_clk_mask0.bits.p2_r1
      = color_clk_mask0.bits.p2_g1
      = color_clk_mask0.bits.p2_b1
      = color_clk_mask0.bits.p2_r2
      = color_clk_mask0.bits.p2_g2
      = color_clk_mask0.bits.p2_b2 = 1;
  }
#endif

#ifdef CM_5_CHAIN_SUPPORT
  if (parallel_ >= 4) {
    color_clk_mask1.bits.p3_r1
      = color_clk_mask1.bits.p3_g1
      = color_clk_mask1.bits.p3_b1
      = color_clk_mask1.bits.p3_r2
      = color_clk_mask1.bits.p3_g2
      = color_clk_mask1.bits.p3_b2 = 1;
  }

  if (parallel_ >= 5) {
    color_clk_mask1.bits.p4_r1
      = color_clk_mask1.bits.p4_g1
      = color_clk_mask1.bits.p4_b1
      = color_clk_mask1.bits.p4_r2
      = color_clk_mask1.bits.p4_g2
      = color_clk_mask1.bits.p4_b2 = 1;
  }
#endif


#ifdef PI_REV1_RGB_PINOUT_
  color_clk_mask0.bits.clock_rev1 = color_clk_mask0.bits.clock_rev2 = 1;
#endif
  color_clk_mask0.bits.clock = 1;

  IoBits0 row_mask;
  row_mask.bits.a = row_mask.bits.b = row_mask.bits.c
    = row_mask.bits.d = row_mask.bits.e = 1;

  IoBits0 clock, strobe, row_address;
#ifdef PI_REV1_RGB_PINOUT_
  clock.bits.clock_rev1 = clock.bits.clock_rev2 = 1;
#endif
  clock.bits.clock = 1;
  strobe.bits.strobe = 1;

  const int pwm_to_show = pwm_bits_;  // Local copy, might change in process.
  for (uint8_t d_row = 0; d_row < double_rows_; ++d_row) {
    row_address.bits.a = d_row;
    row_address.bits.b = d_row >> 1;
    row_address.bits.c = d_row >> 2;
    row_address.bits.d = d_row >> 3;
    row_address.bits.e = d_row >> 4;

    io->WriteMaskedBits(row_address.raw, row_mask.raw, 0, 0);  // Set row address

    // Rows can't be switched very quickly without ghosting, so we do the
    // full PWM of one row before switching rows.
    for (int b = kBitPlanes - pwm_to_show; b < kBitPlanes; ++b) {
      IoBits0 *row_data0 = ValueAt_0(d_row, 0, b);
#ifdef CM_5_CHAIN_SUPPORT
      IoBits1 *row_data1 = ValueAt_1(d_row, 0, b);
#endif
      // While the output enable is still on, we can already clock in the next
      // data.
      for (int col = 0; col < columns_; ++col) {
        const IoBits0 &out0 = *row_data0++;
#ifdef CM_5_CHAIN_SUPPORT
        const IoBits1 &out1 = *row_data1++;
        io->WriteMaskedBits(out0.raw, color_clk_mask0.raw, out1.raw, color_clk_mask1.raw);  // col + reset clock
#else
        io->WriteMaskedBits(out0.raw, color_clk_mask0.raw, 0, 0);  // col + reset clock
#endif
        io->SetBits(clock.raw,0);               // Rising edge: clock color in.
      }
#ifdef CM_5_CHAIN_SUPPORT
      io->ClearBits(color_clk_mask0.raw,color_clk_mask1.raw);    // clock back to normal.
#else
      io->ClearBits(color_clk_mask0.raw,0);    // clock back to normal.
#endif
      // OE of the previous row-data must be finished before strobe.
      sOutputEnablePulser->WaitPulseFinished();

      io->SetBits(strobe.raw,0);   // Strobe in the previously clocked in row.
      io->ClearBits(strobe.raw,0);

      // Now switch on for the sleep time necessary for that bit-plane.
      sOutputEnablePulser->SendPulse(b);
    }
    sOutputEnablePulser->WaitPulseFinished();
  }
}
}  // namespace internal
}  // namespace rgb_matrix
