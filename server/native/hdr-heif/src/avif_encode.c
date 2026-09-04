// SPDX-License-Identifier: AGPL-3.0-or-later
//
// 10-bit PQ AVIF encoding via libavif.
//
// The input is relative linear light (1.0 == diffuse SDR white) in the source
// primaries. We convert to BT.2020 primaries and encode with the SMPTE ST 2084
// (PQ) transfer function, which is what Safari and Chrome expect for HDR AVIF.
//
// The BT.2020 tag is honest rather than cosmetic: we perform a real
// colorimetric conversion from the detected source primaries, so the pixel data
// genuinely lives in BT.2020. Content light level is measured from the actual
// reconstructed pixels rather than assumed.

#include "hdr_heif.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
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

bool hdr_encode_avif(const hdr_linear_image *image, const char *output_path,
                     const hdr_convert_options *options, hdr_convert_result *result, char *error) {
  if (image->pixels == NULL || image->width == 0 || image->height == 0) {
    set_error(error, "no pixel data to encode");
    return false;
  }

  double to_bt2020[9];
  if (!hdr_primaries_matrix(image->primaries, HDR_PRIMARIES_BT2020, to_bt2020)) {
    set_error(error, "unsupported source primaries '%s'", hdr_primaries_name(image->primaries));
    return false;
  }

  avifImage *avif = avifImageCreate((uint32_t)image->width, (uint32_t)image->height, 10,
                                    AVIF_PIXEL_FORMAT_YUV444);
  if (avif == NULL) {
    set_error(error, "avifImageCreate failed");
    return false;
  }

  avif->colorPrimaries = AVIF_COLOR_PRIMARIES_BT2020;
  avif->transferCharacteristics = AVIF_TRANSFER_CHARACTERISTICS_PQ;
  avif->matrixCoefficients = AVIF_MATRIX_COEFFICIENTS_BT2020_NCL;
  avif->yuvRange = AVIF_RANGE_FULL;

  avifRGBImage rgb;
  avifRGBImageSetDefaults(&rgb, avif);
  rgb.format = AVIF_RGB_FORMAT_RGB;
  rgb.depth = 10;
  if (avifRGBImageAllocatePixels(&rgb) != AVIF_RESULT_OK) {
    set_error(error, "failed to allocate RGB staging buffer");
    avifImageDestroy(avif);
    return false;
  }

  const double sdr_white = options->sdr_white_nits;
  const double max_code = 1023.0;

  double max_component_nits = 0.0; // -> MaxCLL
  double max_frame_average = 0.0;  // -> MaxPALL
  double frame_sum = 0.0;

  for (uint32_t y = 0; y < image->height; y++) {
    uint16_t *dst = (uint16_t *)(void *)(rgb.pixels + (size_t)y * rgb.rowBytes);

    for (uint32_t x = 0; x < image->width; x++) {
      const float *src = image->pixels + (((size_t)y * image->width) + x) * 3;

      // Source primaries -> BT.2020, in linear light.
      double out[3];
      for (int c = 0; c < 3; c++) {
        out[c] = to_bt2020[c * 3 + 0] * src[0] + to_bt2020[c * 3 + 1] * src[1] +
                 to_bt2020[c * 3 + 2] * src[2];
        if (out[c] < 0.0) {
          out[c] = 0.0; // clip the small negatives a gamut change can produce
        }
      }

      // Relative light -> absolute nits -> PQ code value.
      double pixel_peak = 0.0;
      for (int c = 0; c < 3; c++) {
        const double nits = out[c] * sdr_white;
        if (nits > pixel_peak) {
          pixel_peak = nits;
        }
        const double code = hdr_pq_inverse_eotf(nits / 10000.0) * max_code;
        int value = (int)(code + 0.5);
        if (value < 0) {
          value = 0;
        }
        if (value > 1023) {
          value = 1023;
        }
        dst[(size_t)x * 3 + c] = (uint16_t)value;
      }

      if (pixel_peak > max_component_nits) {
        max_component_nits = pixel_peak;
      }
      // BT.2020 luminance for the frame average.
      frame_sum += (0.2627 * out[0] + 0.6780 * out[1] + 0.0593 * out[2]) * sdr_white;
    }
  }

  const double pixel_count = (double)image->width * (double)image->height;
  max_frame_average = (pixel_count > 0.0) ? frame_sum / pixel_count : 0.0;

  if (avifImageRGBToYUV(avif, &rgb) != AVIF_RESULT_OK) {
    set_error(error, "RGB to YUV conversion failed");
    avifRGBImageFreePixels(&rgb);
    avifImageDestroy(avif);
    return false;
  }
  avifRGBImageFreePixels(&rgb);

  // Content light level, measured from the pixels we just wrote.
  avif->clli.maxCLL = (uint16_t)(max_component_nits > 65535.0 ? 65535.0 : max_component_nits + 0.5);
  avif->clli.maxPALL = (uint16_t)(max_frame_average > 65535.0 ? 65535.0 : max_frame_average + 0.5);

  avifEncoder *encoder = avifEncoderCreate();
  if (encoder == NULL) {
    set_error(error, "avifEncoderCreate failed");
    avifImageDestroy(avif);
    return false;
  }
  encoder->quality = options->quality;
  encoder->speed = options->speed;

  avifRWData output = AVIF_DATA_EMPTY;
  const avifResult encode_result = avifEncoderWrite(encoder, avif, &output);
  avifEncoderDestroy(encoder);

  if (encode_result != AVIF_RESULT_OK) {
    set_error(error, "AVIF encode failed: %s", avifResultToString(encode_result));
    avifRWDataFree(&output);
    avifImageDestroy(avif);
    return false;
  }

  FILE *fp = fopen(output_path, "wb");
  if (fp == NULL) {
    set_error(error, "could not open '%s' for writing", output_path);
    avifRWDataFree(&output);
    avifImageDestroy(avif);
    return false;
  }
  const size_t written = fwrite(output.data, 1, output.size, fp);
  const bool flushed = fclose(fp) == 0;

  if (written != output.size || !flushed) {
    set_error(error, "short write to '%s'", output_path);
    avifRWDataFree(&output);
    avifImageDestroy(avif);
    return false;
  }

  result->width = image->width;
  result->height = image->height;
  result->bit_depth = 10;
  result->out_primaries_cicp = AVIF_COLOR_PRIMARIES_BT2020;
  result->out_transfer_cicp = AVIF_TRANSFER_CHARACTERISTICS_PQ;
  result->out_matrix_cicp = AVIF_MATRIX_COEFFICIENTS_BT2020_NCL;
  result->max_cll = avif->clli.maxCLL;
  result->max_pall = avif->clli.maxPALL;
  result->bytes_written = output.size;

  avifRWDataFree(&output);
  avifImageDestroy(avif);
  return true;
}
