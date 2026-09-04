// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Verification support: decode an AVIF back and measure what is actually in it.
//
// This exists so the test suite can assert that a generated preview is
// genuinely HDR rather than merely *tagged* as HDR. A 10-bit AVIF carrying only
// SDR luminance is a real failure mode, so we measure the decoded PQ code values
// and report the share of pixels that exceed diffuse SDR white.

#include "hdr_heif.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <avif/avif.h>

static void set_error(char *error, const char *fmt, ...) {
  if (error == NULL) {
    return;
  }
  va_list args;
  va_start(args, fmt);
  vsnprintf(error, HDR_ERROR_MAX, fmt, args);
  va_end(args);
}

bool hdr_inspect_avif(const char *path, double sdr_white_nits, hdr_inspect_result *out,
                      char *error) {
  memset(out, 0, sizeof(*out));

  avifDecoder *decoder = avifDecoderCreate();
  if (decoder == NULL) {
    set_error(error, "avifDecoderCreate failed");
    return false;
  }

  avifResult res = avifDecoderSetIOFile(decoder, path);
  if (res != AVIF_RESULT_OK) {
    set_error(error, "could not open '%s': %s", path, avifResultToString(res));
    avifDecoderDestroy(decoder);
    return false;
  }

  res = avifDecoderParse(decoder);
  if (res != AVIF_RESULT_OK) {
    set_error(error, "AVIF parse failed: %s", avifResultToString(res));
    avifDecoderDestroy(decoder);
    return false;
  }

  res = avifDecoderNextImage(decoder);
  if (res != AVIF_RESULT_OK) {
    set_error(error, "AVIF decode failed: %s", avifResultToString(res));
    avifDecoderDestroy(decoder);
    return false;
  }

  const avifImage *image = decoder->image;
  out->width = image->width;
  out->height = image->height;
  out->bit_depth = (int)image->depth;
  out->color_primaries = (uint16_t)image->colorPrimaries;
  out->transfer_characteristics = (uint16_t)image->transferCharacteristics;
  out->matrix_coefficients = (uint16_t)image->matrixCoefficients;
  out->max_cll = image->clli.maxCLL;
  out->max_pall = image->clli.maxPALL;

  const bool is_pq = image->transferCharacteristics == AVIF_TRANSFER_CHARACTERISTICS_PQ;
  const bool is_hlg = image->transferCharacteristics == AVIF_TRANSFER_CHARACTERISTICS_HLG;
  out->hdr_transfer = is_pq || is_hlg;

  // Pull the pixels back out as RGB at the image's own depth.
  avifRGBImage rgb;
  avifRGBImageSetDefaults(&rgb, image);
  rgb.format = AVIF_RGB_FORMAT_RGB;
  rgb.depth = image->depth;
  if (avifRGBImageAllocatePixels(&rgb) != AVIF_RESULT_OK) {
    set_error(error, "failed to allocate RGB buffer for inspection");
    avifDecoderDestroy(decoder);
    return false;
  }
  res = avifImageYUVToRGB(image, &rgb);
  if (res != AVIF_RESULT_OK) {
    set_error(error, "YUV to RGB conversion failed: %s", avifResultToString(res));
    avifRGBImageFreePixels(&rgb);
    avifDecoderDestroy(decoder);
    return false;
  }

  const double max_code = (double)((1u << image->depth) - 1u);
  const double sdr_white = (sdr_white_nits > 0.0) ? sdr_white_nits : 203.0;

  double peak_nits = 0.0;
  uint64_t above_sdr = 0;
  uint64_t total = 0;
  uint32_t distinct_low_bits = 0; // used to spot an 8-bit image padded to 10-bit

  for (uint32_t y = 0; y < rgb.height; y++) {
    const uint8_t *row = rgb.pixels + (size_t)y * rgb.rowBytes;

    for (uint32_t x = 0; x < rgb.width; x++) {
      double channel_peak = 0.0;

      for (int c = 0; c < 3; c++) {
        double value;
        if (image->depth > 8) {
          const uint16_t raw = ((const uint16_t *)(const void *)row)[(size_t)x * 3 + c];
          distinct_low_bits |= (uint32_t)(raw & 0x3u);
          value = (double)raw / max_code;
        } else {
          value = (double)row[(size_t)x * 3 + c] / max_code;
        }

        double nits;
        if (is_pq) {
          nits = hdr_pq_eotf(value) * 10000.0;
        } else if (is_hlg) {
          nits = hdr_hlg_oetf_inverse(value) * 1000.0;
        } else {
          nits = hdr_srgb_eotf(value) * sdr_white;
        }

        if (nits > channel_peak) {
          channel_peak = nits;
        }
      }

      if (channel_peak > peak_nits) {
        peak_nits = channel_peak;
      }
      // Allow a small tolerance so quantisation alone cannot count as HDR.
      if (channel_peak > sdr_white * 1.05) {
        above_sdr++;
      }
      total++;
    }
  }

  avifRGBImageFreePixels(&rgb);
  avifDecoderDestroy(decoder);

  out->peak_nits = peak_nits;
  out->fraction_above_sdr_white = (total > 0) ? (double)above_sdr / (double)total : 0.0;
  out->uses_sub_8bit_precision = distinct_low_bits != 0;

  // Genuinely HDR means: an HDR transfer function, more than 8 bits, and pixel
  // values that actually exceed diffuse SDR white.
  out->genuinely_hdr = out->hdr_transfer && out->bit_depth > 8 && peak_nits > sdr_white * 1.05;

  return true;
}
