// SPDX-License-Identifier: AGPL-3.0-or-later
//
// CLI entry point for the Immich HDR HEIF -> AVIF helper.
//
//   immich-hdr-heif probe   <input.heic>
//   immich-hdr-heif convert <input.heic> <output.avif> [options]
//
// Both subcommands print a single line of JSON to stdout. Diagnostics go to
// stderr so the caller can parse stdout unconditionally. The exit code is 0 on
// success and non-zero on failure; `probe` treats "this file is not HDR" as a
// success with `"hdr": false`, because that is a normal answer rather than an
// error.

#include "hdr_heif.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void print_usage(FILE *out) {
  fprintf(out,
          "Usage:\n"
          "  immich-hdr-heif probe   <input>\n"
          "  immich-hdr-heif convert <input> <output.avif> [options]\n"
          "  immich-hdr-heif inspect <input.avif> [--sdr-white N]\n"
          "\n"
          "Options for convert:\n"
          "  --size N          bound the longest edge (default 1440, 0 = native)\n"
          "  --quality N       AVIF quality 0-100 (default 65)\n"
          "  --speed N         AVIF encoder speed 0-10 (default 6)\n"
          "  --sdr-white N     nits assigned to diffuse SDR white (default 203)\n");
}

// Emit a JSON string with the handful of escapes we can actually encounter.
static void print_json_string(const char *value) {
  putchar('"');
  for (const char *p = value; *p != '\0'; p++) {
    switch (*p) {
      case '"':
        fputs("\\\"", stdout);
        break;
      case '\\':
        fputs("\\\\", stdout);
        break;
      case '\n':
        fputs("\\n", stdout);
        break;
      case '\r':
        fputs("\\r", stdout);
        break;
      case '\t':
        fputs("\\t", stdout);
        break;
      default:
        if ((unsigned char)*p < 0x20) {
          printf("\\u%04x", (unsigned char)*p);
        } else {
          putchar(*p);
        }
    }
  }
  putchar('"');
}

static void print_error_json(const char *message) {
  fputs("{\"ok\":false,\"error\":", stdout);
  print_json_string(message);
  fputs("}\n", stdout);
}

static void print_probe_json(const hdr_probe_result *probe) {
  printf("{\"ok\":true,\"hdr\":%s,\"mode\":", probe->hdr ? "true" : "false");
  print_json_string(hdr_mode_name(probe->mode));
  printf(",\"width\":%u,\"height\":%u,\"bitDepth\":%d", probe->width, probe->height,
         probe->bit_depth);
  printf(",\"hasGainMap\":%s", probe->has_gain_map ? "true" : "false");
  printf(",\"hasTmap\":%s", probe->has_tmap_item ? "true" : "false");
  printf(",\"gainMapHeadroom\":%.6f", probe->gain_map_headroom);
  fputs(",\"headroomSource\":", stdout);
  print_json_string(probe->headroom_source);
  fputs(",\"sourcePrimaries\":", stdout);
  print_json_string(hdr_primaries_name(probe->primaries));
  fputs(",\"sourceTransfer\":", stdout);
  print_json_string(hdr_transfer_name(probe->transfer));
  fputs(",\"auxGainMapType\":", stdout);
  print_json_string(probe->aux_gain_map_type);
  fputs("}\n", stdout);
}

static void print_convert_json(const hdr_convert_result *result) {
  fputs("{\"ok\":true,\"format\":\"avif\",\"hdr\":true", stdout);
  printf(",\"width\":%u,\"height\":%u,\"bitDepth\":%d", result->width, result->height,
         result->bit_depth);
  fputs(",\"mode\":", stdout);
  print_json_string(hdr_mode_name(result->mode));
  fputs(",\"colorPrimaries\":\"bt2020\",\"transferCharacteristics\":\"pq\"", stdout);
  fputs(",\"matrixCoefficients\":\"bt2020-ncl\"", stdout);
  printf(",\"colorPrimariesCicp\":%u,\"transferCharacteristicsCicp\":%u,\"matrixCoefficientsCicp\":%u",
         result->out_primaries_cicp, result->out_transfer_cicp, result->out_matrix_cicp);
  printf(",\"maxCll\":%u,\"maxPall\":%u", result->max_cll, result->max_pall);
  printf(",\"gainMapHeadroom\":%.6f", result->gain_map_headroom);
  fputs(",\"sourcePrimaries\":", stdout);
  print_json_string(hdr_primaries_name(result->source_primaries));
  fputs(",\"sourceTransfer\":", stdout);
  print_json_string(hdr_transfer_name(result->source_transfer));
  printf(",\"bytes\":%zu", result->bytes_written);
  fputs("}\n", stdout);
}

static bool parse_int_arg(const char *value, long *out) {
  char *end = NULL;
  const long parsed = strtol(value, &end, 10);
  if (end == value || *end != '\0') {
    return false;
  }
  *out = parsed;
  return true;
}

static int run_probe(const char *path) {
  char error[HDR_ERROR_MAX] = {0};
  hdr_probe_result probe;

  if (!hdr_probe_file(path, &probe, error)) {
    // Detection failure is reported, but the caller is expected to fall back to
    // the ordinary SDR pipeline rather than treat this as fatal.
    print_error_json(error[0] != '\0' ? error : "probe failed");
    return 2;
  }

  print_probe_json(&probe);
  return 0;
}

static int run_convert(int argc, char **argv) {
  if (argc < 4) {
    print_usage(stderr);
    return 1;
  }

  const char *input = argv[2];
  const char *output = argv[3];

  hdr_convert_options options;
  hdr_convert_options_defaults(&options);

  for (int i = 4; i < argc; i++) {
    const bool has_value = i + 1 < argc;
    long value = 0;

    if (strcmp(argv[i], "--size") == 0 && has_value && parse_int_arg(argv[++i], &value)) {
      options.max_size = (uint32_t)(value < 0 ? 0 : value);
    } else if (strcmp(argv[i], "--quality") == 0 && has_value && parse_int_arg(argv[++i], &value)) {
      options.quality = (int)(value < 0 ? 0 : (value > 100 ? 100 : value));
    } else if (strcmp(argv[i], "--speed") == 0 && has_value && parse_int_arg(argv[++i], &value)) {
      options.speed = (int)(value < 0 ? 0 : (value > 10 ? 10 : value));
    } else if (strcmp(argv[i], "--sdr-white") == 0 && has_value) {
      options.sdr_white_nits = strtod(argv[++i], NULL);
      if (options.sdr_white_nits <= 0.0) {
        options.sdr_white_nits = 203.0;
      }
    } else {
      fprintf(stderr, "unknown or malformed option: %s\n", argv[i]);
      print_usage(stderr);
      return 1;
    }
  }

  char error[HDR_ERROR_MAX] = {0};
  hdr_probe_result probe;
  if (!hdr_probe_file(input, &probe, error)) {
    print_error_json(error[0] != '\0' ? error : "probe failed");
    return 2;
  }

  if (!probe.hdr) {
    print_error_json("source is not HDR; use the standard preview pipeline");
    return 3;
  }

  hdr_linear_image image;
  if (!hdr_decode_linear(input, &probe, &options, &image, error)) {
    print_error_json(error[0] != '\0' ? error : "decode failed");
    return 4;
  }

  hdr_convert_result result;
  memset(&result, 0, sizeof(result));
  result.mode = probe.mode;
  result.gain_map_headroom = probe.gain_map_headroom;
  result.headroom_source = probe.headroom_source;
  result.source_primaries = probe.primaries;
  result.source_transfer = probe.transfer;

  const bool encoded = hdr_encode_avif(&image, output, &options, &result, error);
  hdr_linear_image_free(&image);

  if (!encoded) {
    print_error_json(error[0] != '\0' ? error : "encode failed");
    return 5;
  }

  print_convert_json(&result);
  return 0;
}

static int run_inspect(const char *path, double sdr_white) {
  char error[HDR_ERROR_MAX] = {0};
  hdr_inspect_result inspect;

  if (!hdr_inspect_avif(path, sdr_white, &inspect, error)) {
    print_error_json(error[0] != '\0' ? error : "inspect failed");
    return 2;
  }

  printf("{\"ok\":true,\"width\":%u,\"height\":%u,\"bitDepth\":%d", inspect.width, inspect.height,
         inspect.bit_depth);
  printf(",\"colorPrimariesCicp\":%u,\"transferCharacteristicsCicp\":%u,\"matrixCoefficientsCicp\":%u",
         inspect.color_primaries, inspect.transfer_characteristics, inspect.matrix_coefficients);
  printf(",\"maxCll\":%u,\"maxPall\":%u", inspect.max_cll, inspect.max_pall);
  printf(",\"hdrTransfer\":%s", inspect.hdr_transfer ? "true" : "false");
  printf(",\"genuinelyHdr\":%s", inspect.genuinely_hdr ? "true" : "false");
  printf(",\"usesSub8BitPrecision\":%s", inspect.uses_sub_8bit_precision ? "true" : "false");
  printf(",\"peakNits\":%.3f", inspect.peak_nits);
  printf(",\"fractionAboveSdrWhite\":%.6f", inspect.fraction_above_sdr_white);
  fputs("}\n", stdout);
  return 0;
}

static int run_dump_linear(int argc, char **argv) {
  if (argc < 4) {
    print_usage(stderr);
    return 1;
  }

  const char *input = argv[2];
  const char *output = argv[3];

  hdr_convert_options options;
  hdr_convert_options_defaults(&options);
  bool disable_gain_map = false;
  for (int i = 4; i < argc; i++) {
    long value = 0;
    if (strcmp(argv[i], "--no-gain-map") == 0) {
      disable_gain_map = true;
    } else if (strcmp(argv[i], "--size") == 0 && i + 1 < argc &&
               parse_int_arg(argv[i + 1], &value)) {
      options.max_size = (uint32_t)(value < 0 ? 0 : value);
      i++;
    }
  }

  char error[HDR_ERROR_MAX] = {0};
  hdr_probe_result probe;
  if (!hdr_probe_file(input, &probe, error) || !probe.hdr) {
    print_error_json(error[0] != '\0' ? error : "source is not HDR");
    return 2;
  }

  if (disable_gain_map) {
    // Reconstruct the SDR base only, so it can be diffed against a reference
    // SDR render to separate base-rendering differences from gain-map ones.
    probe.mode = HDR_MODE_NATIVE_HDR;
    probe.gain_map_headroom = 1.0;
  }

  hdr_linear_image image;
  if (!hdr_decode_linear(input, &probe, &options, &image, error)) {
    print_error_json(error[0] != '\0' ? error : "decode failed");
    return 4;
  }

  FILE *fp = fopen(output, "wb");
  if (fp == NULL) {
    hdr_linear_image_free(&image);
    print_error_json("could not open dump target");
    return 5;
  }
  const size_t count = (size_t)image.width * image.height * 3;
  const size_t written = fwrite(image.pixels, sizeof(float), count, fp);
  fclose(fp);

  printf("{\"ok\":true,\"width\":%u,\"height\":%u,\"channels\":3,\"floats\":%zu,\"primaries\":",
         image.width, image.height, written);
  print_json_string(hdr_primaries_name(image.primaries));
  fputs("}\n", stdout);

  hdr_linear_image_free(&image);
  return written == count ? 0 : 6;
}

int main(int argc, char **argv) {
  if (argc < 2) {
    print_usage(stderr);
    return 1;
  }

  if (strcmp(argv[1], "probe") == 0) {
    if (argc < 3) {
      print_usage(stderr);
      return 1;
    }
    return run_probe(argv[2]);
  }

  if (strcmp(argv[1], "convert") == 0) {
    return run_convert(argc, argv);
  }

  if (strcmp(argv[1], "inspect") == 0) {
    if (argc < 3) {
      print_usage(stderr);
      return 1;
    }
    double sdr_white = 203.0;
    if (argc >= 5 && strcmp(argv[3], "--sdr-white") == 0) {
      sdr_white = strtod(argv[4], NULL);
    }
    return run_inspect(argv[2], sdr_white);
  }

  // Debug aid: write the reconstructed linear buffer (source primaries,
  // 1.0 == diffuse SDR white) as raw float32 RGB for comparison against a
  // reference renderer.
  if (strcmp(argv[1], "dump-linear") == 0) {
    return run_dump_linear(argc, argv);
  }

  if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
    print_usage(stdout);
    return 0;
  }

  print_usage(stderr);
  return 1;
}
