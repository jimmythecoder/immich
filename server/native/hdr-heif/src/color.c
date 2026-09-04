// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Colour transfer functions and primary conversions.
//
// Matrices are derived at runtime from chromaticity coordinates rather than
// hardcoded, so adding a colour space only means adding its primaries.

#include "hdr_heif.h"

#include <math.h>
#include <string.h>

// -- transfer functions -----------------------------------------------------

double hdr_srgb_eotf(double v) {
  if (v <= 0.04045) {
    return v / 12.92;
  }
  return pow((v + 0.055) / 1.055, 2.4);
}

// SMPTE ST 2084. Returns luminance normalised so 1.0 == 10000 nits.
double hdr_pq_eotf(double v) {
  static const double m1 = 2610.0 / 16384.0;
  static const double m2 = 2523.0 / 4096.0 * 128.0;
  static const double c1 = 3424.0 / 4096.0;
  static const double c2 = 2413.0 / 4096.0 * 32.0;
  static const double c3 = 2392.0 / 4096.0 * 32.0;

  if (v <= 0.0) {
    return 0.0;
  }
  const double vm = pow(v, 1.0 / m2);
  double num = vm - c1;
  if (num < 0.0) {
    num = 0.0;
  }
  const double den = c2 - c3 * vm;
  if (den <= 0.0) {
    return 1.0;
  }
  return pow(num / den, 1.0 / m1);
}

// Inverse of the above: absolute luminance (1.0 == 10000 nits) -> PQ code.
double hdr_pq_inverse_eotf(double l) {
  static const double m1 = 2610.0 / 16384.0;
  static const double m2 = 2523.0 / 4096.0 * 128.0;
  static const double c1 = 3424.0 / 4096.0;
  static const double c2 = 2413.0 / 4096.0 * 32.0;
  static const double c3 = 2392.0 / 4096.0 * 32.0;

  if (l <= 0.0) {
    return 0.0;
  }
  if (l > 1.0) {
    l = 1.0;
  }
  const double lm = pow(l, m1);
  return pow((c1 + c2 * lm) / (1.0 + c3 * lm), m2);
}

// ARIB STD-B67 inverse OETF; returns scene light normalised to [0, 1].
double hdr_hlg_oetf_inverse(double v) {
  static const double a = 0.17883277;
  static const double b = 0.28466892; // 1 - 4a
  static const double c = 0.55991073; // 0.5 - a*ln(4a)

  if (v <= 0.0) {
    return 0.0;
  }
  if (v <= 0.5) {
    return (v * v) / 3.0;
  }
  return (exp((v - c) / a) + b) / 12.0;
}

// -- primaries --------------------------------------------------------------

typedef struct {
  double rx, ry;
  double gx, gy;
  double bx, by;
  double wx, wy;
} chromaticities;

static bool lookup_chromaticities(hdr_primaries p, chromaticities *out) {
  switch (p) {
    case HDR_PRIMARIES_SRGB:
      *out = (chromaticities){0.640, 0.330, 0.300, 0.600, 0.150, 0.060, 0.3127, 0.3290};
      return true;
    case HDR_PRIMARIES_DISPLAY_P3:
      *out = (chromaticities){0.680, 0.320, 0.265, 0.690, 0.150, 0.060, 0.3127, 0.3290};
      return true;
    case HDR_PRIMARIES_BT2020:
      *out = (chromaticities){0.708, 0.292, 0.170, 0.797, 0.131, 0.046, 0.3127, 0.3290};
      return true;
    case HDR_PRIMARIES_ADOBE_RGB:
      *out = (chromaticities){0.640, 0.330, 0.210, 0.710, 0.150, 0.060, 0.3127, 0.3290};
      return true;
    default:
      return false;
  }
}

static bool mat3_inverse(const double m[9], double out[9]) {
  const double det = m[0] * (m[4] * m[8] - m[5] * m[7]) - m[1] * (m[3] * m[8] - m[5] * m[6]) +
                     m[2] * (m[3] * m[7] - m[4] * m[6]);
  if (fabs(det) < 1e-12) {
    return false;
  }
  const double inv = 1.0 / det;

  out[0] = (m[4] * m[8] - m[5] * m[7]) * inv;
  out[1] = (m[2] * m[7] - m[1] * m[8]) * inv;
  out[2] = (m[1] * m[5] - m[2] * m[4]) * inv;
  out[3] = (m[5] * m[6] - m[3] * m[8]) * inv;
  out[4] = (m[0] * m[8] - m[2] * m[6]) * inv;
  out[5] = (m[2] * m[3] - m[0] * m[5]) * inv;
  out[6] = (m[3] * m[7] - m[4] * m[6]) * inv;
  out[7] = (m[1] * m[6] - m[0] * m[7]) * inv;
  out[8] = (m[0] * m[4] - m[1] * m[3]) * inv;
  return true;
}

static void mat3_mul(const double a[9], const double b[9], double out[9]) {
  for (int row = 0; row < 3; row++) {
    for (int col = 0; col < 3; col++) {
      double sum = 0.0;
      for (int k = 0; k < 3; k++) {
        sum += a[row * 3 + k] * b[k * 3 + col];
      }
      out[row * 3 + col] = sum;
    }
  }
}

// Standard derivation (Lindbloom): build RGB->XYZ from chromaticities by
// scaling each primary so that RGB(1,1,1) maps exactly to the white point.
static bool rgb_to_xyz_matrix(hdr_primaries p, double out[9]) {
  chromaticities c;
  if (!lookup_chromaticities(p, &c)) {
    return false;
  }

  const double xr = c.rx / c.ry, yr = 1.0, zr = (1.0 - c.rx - c.ry) / c.ry;
  const double xg = c.gx / c.gy, yg = 1.0, zg = (1.0 - c.gx - c.gy) / c.gy;
  const double xb = c.bx / c.by, yb = 1.0, zb = (1.0 - c.bx - c.by) / c.by;

  const double m[9] = {xr, xg, xb, yr, yg, yb, zr, zg, zb};
  double inv[9];
  if (!mat3_inverse(m, inv)) {
    return false;
  }

  const double xw = c.wx / c.wy;
  const double yw = 1.0;
  const double zw = (1.0 - c.wx - c.wy) / c.wy;

  const double sr = inv[0] * xw + inv[1] * yw + inv[2] * zw;
  const double sg = inv[3] * xw + inv[4] * yw + inv[5] * zw;
  const double sb = inv[6] * xw + inv[7] * yw + inv[8] * zw;

  out[0] = sr * xr;
  out[1] = sg * xg;
  out[2] = sb * xb;
  out[3] = sr * yr;
  out[4] = sg * yg;
  out[5] = sb * yb;
  out[6] = sr * zr;
  out[7] = sg * zg;
  out[8] = sb * zb;
  return true;
}

bool hdr_primaries_matrix(hdr_primaries from, hdr_primaries to, double matrix[9]) {
  if (from == to) {
    static const double identity[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    memcpy(matrix, identity, sizeof(identity));
    return true;
  }

  double from_xyz[9];
  double to_xyz[9];
  double to_rgb[9];
  if (!rgb_to_xyz_matrix(from, from_xyz) || !rgb_to_xyz_matrix(to, to_xyz)) {
    return false;
  }
  if (!mat3_inverse(to_xyz, to_rgb)) {
    return false;
  }

  // Both spaces use D65, so no chromatic adaptation is required.
  mat3_mul(to_rgb, from_xyz, matrix);
  return true;
}

// -- names ------------------------------------------------------------------

const char *hdr_primaries_name(hdr_primaries p) {
  switch (p) {
    case HDR_PRIMARIES_SRGB:
      return "srgb";
    case HDR_PRIMARIES_DISPLAY_P3:
      return "display-p3";
    case HDR_PRIMARIES_BT2020:
      return "bt2020";
    case HDR_PRIMARIES_ADOBE_RGB:
      return "adobe-rgb";
    default:
      return "unknown";
  }
}

const char *hdr_transfer_name(hdr_transfer t) {
  switch (t) {
    case HDR_TRANSFER_SRGB:
      return "srgb";
    case HDR_TRANSFER_PQ:
      return "pq";
    case HDR_TRANSFER_HLG:
      return "hlg";
    default:
      return "unknown";
  }
}

const char *hdr_mode_name(hdr_mode m) {
  switch (m) {
    case HDR_MODE_APPLE_GAIN_MAP:
      return "apple-gain-map";
    case HDR_MODE_NATIVE_HDR:
      return "native-hdr";
    default:
      return "none";
  }
}

uint16_t hdr_primaries_cicp(hdr_primaries p) {
  switch (p) {
    case HDR_PRIMARIES_DISPLAY_P3:
      return 12; // SMPTE EG 432-1
    case HDR_PRIMARIES_BT2020:
      return 9;
    default:
      return 1; // BT.709 / sRGB
  }
}
