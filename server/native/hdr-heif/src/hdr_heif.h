// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Shared types for the Immich HDR HEIF -> AVIF helper.
//
// The helper is intentionally self-contained: it links libheif (decode) and
// libavif (encode) directly and exposes a small CLI so that the Node server can
// drive it through child_process. Everything the server needs to know about a
// conversion is returned as JSON on stdout.

#ifndef IMMICH_HDR_HEIF_H
#define IMMICH_HDR_HEIF_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HDR_ERROR_MAX 512

// Colour primaries we can recognise on the source side. These map onto CICP
// values when we emit metadata.
typedef enum {
  HDR_PRIMARIES_UNKNOWN = 0,
  HDR_PRIMARIES_SRGB,
  HDR_PRIMARIES_DISPLAY_P3,
  HDR_PRIMARIES_BT2020,
  HDR_PRIMARIES_ADOBE_RGB,
} hdr_primaries;

// How the source encodes light. Used to pick the correct EOTF when linearising.
typedef enum {
  HDR_TRANSFER_UNKNOWN = 0,
  HDR_TRANSFER_SRGB,
  HDR_TRANSFER_PQ,
  HDR_TRANSFER_HLG,
} hdr_transfer;

// The route we used to reconstruct HDR from the source file.
typedef enum {
  // Not HDR-capable; caller should fall back to the ordinary SDR pipeline.
  HDR_MODE_NONE = 0,
  // 8-bit SDR base image + Apple auxiliary gain map (+ tmap derived item).
  HDR_MODE_APPLE_GAIN_MAP,
  // Image item is natively >8-bit with a PQ or HLG transfer function.
  HDR_MODE_NATIVE_HDR,
} hdr_mode;

// Result of inspecting a HEIF file without fully decoding it.
typedef struct {
  bool is_heif;
  bool hdr;
  hdr_mode mode;

  uint32_t width;
  uint32_t height;
  int bit_depth;

  bool has_gain_map;
  bool has_tmap_brand;
  bool has_tmap_item;
  // Headroom is a linear multiplier over SDR white (1.0 == no headroom).
  double gain_map_headroom;
  // Where the headroom value came from, for logging/debugging.
  const char *headroom_source;

  hdr_primaries primaries;
  hdr_transfer transfer;

  char aux_gain_map_type[128];
} hdr_probe_result;

// Result of a successful conversion.
typedef struct {
  uint32_t width;
  uint32_t height;
  int bit_depth;
  hdr_mode mode;
  double gain_map_headroom;
  const char *headroom_source;
  hdr_primaries source_primaries;
  hdr_transfer source_transfer;

  // CICP values actually written into the AVIF.
  uint16_t out_primaries_cicp;
  uint16_t out_transfer_cicp;
  uint16_t out_matrix_cicp;

  uint16_t max_cll;
  uint16_t max_pall;
  size_t bytes_written;
} hdr_convert_result;

// Options controlling a conversion.
typedef struct {
  // Longest-edge bound for the generated preview. 0 == keep native size.
  uint32_t max_size;
  // libavif quality, 0 (worst) .. 100 (lossless).
  int quality;
  // libavif speed, 0 (slowest/best) .. 10.
  int speed;
  // Absolute luminance in nits assigned to diffuse SDR white. BT.2408 uses 203.
  double sdr_white_nits;
} hdr_convert_options;

void hdr_convert_options_defaults(hdr_convert_options *options);

// ---------------------------------------------------------------------------
// Decoded, fully reconstructed HDR image.
//
// Pixels are scene-referred linear RGB in the *source* primaries, normalised so
// that 1.0 == diffuse SDR white. Values above 1.0 are genuine HDR highlights.
// ---------------------------------------------------------------------------
typedef struct {
  float *pixels; // width * height * 3, linear RGB
  uint32_t width;
  uint32_t height;
  hdr_primaries primaries;
} hdr_linear_image;

void hdr_linear_image_free(hdr_linear_image *image);

// ---------------------------------------------------------------------------
// heif_source.c
// ---------------------------------------------------------------------------

// Inspect a file. Never fails hard: on error `out->is_heif`/`out->hdr` are false
// and a message is written to `error`.
bool hdr_probe_file(const char *path, hdr_probe_result *out, char *error);

// Decode + reconstruct HDR, downscaling to `options->max_size` on the longest
// edge. Downscaling happens in linear light, so highlights survive correctly.
bool hdr_decode_linear(const char *path, const hdr_probe_result *probe,
                       const hdr_convert_options *options, hdr_linear_image *out, char *error);

// ---------------------------------------------------------------------------
// color.c
// ---------------------------------------------------------------------------

double hdr_srgb_eotf(double v);
double hdr_pq_eotf(double v);       // -> normalised to 10000 nits
double hdr_pq_inverse_eotf(double l); // 1.0 == 10000 nits
double hdr_hlg_oetf_inverse(double v);

// Fill `matrix` with the 3x3 conversion from `from` primaries to `to` primaries
// operating on linear RGB. Returns false if either space is unknown.
bool hdr_primaries_matrix(hdr_primaries from, hdr_primaries to, double matrix[9]);

const char *hdr_primaries_name(hdr_primaries p);
const char *hdr_transfer_name(hdr_transfer t);
const char *hdr_mode_name(hdr_mode m);
uint16_t hdr_primaries_cicp(hdr_primaries p);

// ---------------------------------------------------------------------------
// avif_encode.c
// ---------------------------------------------------------------------------

// Encode a reconstructed HDR image as a 10-bit PQ AVIF.
bool hdr_encode_avif(const hdr_linear_image *image, const char *output_path,
                     const hdr_convert_options *options, hdr_convert_result *result, char *error);

// ---------------------------------------------------------------------------
// avif_inspect.c
// ---------------------------------------------------------------------------

// What an existing AVIF actually contains. Used by the test suite to prove a
// preview is genuinely HDR rather than only tagged as such.
typedef struct {
  uint32_t width;
  uint32_t height;
  int bit_depth;

  uint16_t color_primaries;
  uint16_t transfer_characteristics;
  uint16_t matrix_coefficients;
  uint16_t max_cll;
  uint16_t max_pall;

  bool hdr_transfer;
  bool genuinely_hdr;
  bool uses_sub_8bit_precision;

  double peak_nits;
  double fraction_above_sdr_white;
} hdr_inspect_result;

bool hdr_inspect_avif(const char *path, double sdr_white_nits, hdr_inspect_result *out,
                      char *error);

#endif // IMMICH_HDR_HEIF_H
