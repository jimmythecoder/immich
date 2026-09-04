// SPDX-License-Identifier: AGPL-3.0-or-later
//
// libheif-backed decoding for Apple HDR HEIC/HEIF.
//
// Two HDR routes are supported:
//
//  1. HDR_MODE_APPLE_GAIN_MAP - the common Apple case. The primary item is an
//     ordinary 8-bit SDR image (frequently a `grid` of many HEVC tiles) and an
//     auxiliary item of type `urn:com:apple:photo:2020:aux:hdrgainmap` carries a
//     per-pixel multiplier. Newer files additionally expose a `tmap` derived
//     item that pairs the two. libheif (as of 1.23) decodes the base and the
//     auxiliary image but does *not* apply the gain map, so we do it here.
//
//  2. HDR_MODE_NATIVE_HDR - the item itself is >8-bit and tagged PQ or HLG.
//
// Note on orientation: libheif applies the `irot`/`imir` transformative item
// properties during decode. Apple writes rotation there and leaves EXIF
// Orientation at 1, so we deliberately do not apply EXIF orientation on top -
// doing so would double-rotate.

#include "hdr_heif.h"

#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libheif/heif.h>

#define APPLE_GAIN_MAP_AUX "urn:com:apple:photo:2020:aux:hdrgainmap"

// BT.2408 diffuse white. Used to relate PQ absolute luminance to "SDR white".
#define HDR_REFERENCE_WHITE_NITS 203.0

static void set_error(char *error, const char *fmt, ...) {
  if (error == NULL) {
    return;
  }
  va_list args;
  va_start(args, fmt);
  vsnprintf(error, HDR_ERROR_MAX, fmt, args);
  va_end(args);
}

void hdr_convert_options_defaults(hdr_convert_options *options) {
  options->max_size = 1440;
  options->quality = 65;
  options->speed = 6;
  options->sdr_white_nits = HDR_REFERENCE_WHITE_NITS;
}

void hdr_linear_image_free(hdr_linear_image *image) {
  if (image == NULL) {
    return;
  }
  free(image->pixels);
  image->pixels = NULL;
  image->width = 0;
  image->height = 0;
}

// ---------------------------------------------------------------------------
// ftyp brand scan
// ---------------------------------------------------------------------------

// Look for a compatible brand in the leading `ftyp` box. We parse this by hand
// rather than going through libheif so that a malformed/unknown brand list can
// never abort detection.
static bool file_has_brand(const char *path, const char *brand, bool *is_heif) {
  *is_heif = false;

  FILE *fp = fopen(path, "rb");
  if (fp == NULL) {
    return false;
  }

  uint8_t header[512];
  const size_t read = fread(header, 1, sizeof(header), fp);
  fclose(fp);

  if (read < 16 || memcmp(header + 4, "ftyp", 4) != 0) {
    return false;
  }

  uint32_t box_size = ((uint32_t)header[0] << 24) | ((uint32_t)header[1] << 16) |
                      ((uint32_t)header[2] << 8) | (uint32_t)header[3];
  if (box_size > read) {
    box_size = (uint32_t)read;
  }
  if (box_size < 16) {
    return false;
  }

  *is_heif = true;

  // Major brand at [8,12), minor version at [12,16), compatible brands follow.
  if (memcmp(header + 8, brand, 4) == 0) {
    return true;
  }
  for (uint32_t offset = 16; offset + 4 <= box_size; offset += 4) {
    if (memcmp(header + offset, brand, 4) == 0) {
      return true;
    }
  }
  return false;
}

// ---------------------------------------------------------------------------
// ICC profile -> primaries
// ---------------------------------------------------------------------------

static uint32_t read_be32(const uint8_t *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

// Extract the `desc` tag and classify it. Apple only ever ships a handful of
// profiles in camera output, so name matching is reliable and much simpler than
// reconstructing the matrix from rXYZ/gXYZ/bXYZ.
static hdr_primaries primaries_from_icc(const uint8_t *icc, size_t size) {
  if (icc == NULL || size < 132) {
    return HDR_PRIMARIES_UNKNOWN;
  }

  const uint32_t tag_count = read_be32(icc + 128);
  if (tag_count > 512) {
    return HDR_PRIMARIES_UNKNOWN;
  }

  for (uint32_t i = 0; i < tag_count; i++) {
    const size_t entry = 132 + (size_t)i * 12;
    if (entry + 12 > size) {
      break;
    }
    if (memcmp(icc + entry, "desc", 4) != 0) {
      continue;
    }

    const uint32_t offset = read_be32(icc + entry + 4);
    const uint32_t length = read_be32(icc + entry + 8);
    if ((size_t)offset + length > size || length < 12) {
      break;
    }

    // Search the tag payload for a known profile name. This covers both the
    // ICC v2 `desc` (ASCII) and v4 `mluc` (UTF-16BE) encodings, because the
    // ASCII characters we look for appear in both once nulls are ignored.
    char ascii[256];
    size_t n = 0;
    for (uint32_t k = 8; k < length && n + 1 < sizeof(ascii); k++) {
      const uint8_t c = icc[offset + k];
      if (c >= 0x20 && c < 0x7f) {
        ascii[n++] = (char)tolower(c);
      }
    }
    ascii[n] = '\0';

    if (strstr(ascii, "display p3") != NULL || strstr(ascii, "displayp3") != NULL) {
      return HDR_PRIMARIES_DISPLAY_P3;
    }
    if (strstr(ascii, "2020") != NULL || strstr(ascii, "rec. 2020") != NULL) {
      return HDR_PRIMARIES_BT2020;
    }
    if (strstr(ascii, "adobe rgb") != NULL) {
      return HDR_PRIMARIES_ADOBE_RGB;
    }
    if (strstr(ascii, "srgb") != NULL) {
      return HDR_PRIMARIES_SRGB;
    }
    break;
  }

  return HDR_PRIMARIES_UNKNOWN;
}

// ---------------------------------------------------------------------------
// Metadata extraction (XMP headroom + Apple MakerNote fallback)
// ---------------------------------------------------------------------------

// Pull a floating point value that follows `key` in an XMP packet, handling both
// attribute (`key="1.5"`) and element (`<key>1.5</key>`) forms.
static bool xmp_find_double(const uint8_t *data, size_t size, const char *key, double *out) {
  const size_t key_len = strlen(key);
  if (size < key_len) {
    return false;
  }

  for (size_t i = 0; i + key_len <= size; i++) {
    if (memcmp(data + i, key, key_len) != 0) {
      continue;
    }

    // Skip the delimiters between the key and its value, then parse.
    size_t j = i + key_len;
    const size_t limit = (j + 64 < size) ? j + 64 : size;
    while (j < limit) {
      const char c = (char)data[j];
      if ((c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.') {
        break;
      }
      if (c != '=' && c != '"' && c != '>' && c != '\'' && c != ' ' && c != '\t' && c != '\n' &&
          c != '\r') {
        j = limit; // unexpected content - this was not the value we wanted
        break;
      }
      j++;
    }
    if (j >= limit) {
      continue;
    }

    char buffer[64];
    size_t n = 0;
    while (j < size && n + 1 < sizeof(buffer)) {
      const char c = (char)data[j];
      if ((c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.' || c == 'e' || c == 'E') {
        buffer[n++] = c;
        j++;
        continue;
      }
      break;
    }
    buffer[n] = '\0';
    if (n == 0) {
      continue;
    }

    char *end = NULL;
    const double value = strtod(buffer, &end);
    if (end != buffer) {
      *out = value;
      return true;
    }
  }

  return false;
}

// Iterate every metadata block attached to `handle`, invoking `visit` with the
// payload. Returns true as soon as `visit` reports success.
typedef bool (*metadata_visitor)(const uint8_t *data, size_t size, void *context);

static bool visit_metadata(const struct heif_image_handle *handle, const char *type_filter,
                           metadata_visitor visit, void *context) {
  const int count = heif_image_handle_get_number_of_metadata_blocks(handle, type_filter);
  if (count <= 0) {
    return false;
  }

  heif_item_id *ids = calloc((size_t)count, sizeof(heif_item_id));
  if (ids == NULL) {
    return false;
  }
  const int found = heif_image_handle_get_list_of_metadata_block_IDs(handle, type_filter, ids, count);

  bool result = false;
  for (int i = 0; i < found && !result; i++) {
    const size_t size = heif_image_handle_get_metadata_size(handle, ids[i]);
    if (size == 0 || size > (64u << 20)) {
      continue;
    }
    uint8_t *data = malloc(size);
    if (data == NULL) {
      continue;
    }
    const struct heif_error err = heif_image_handle_get_metadata(handle, ids[i], data);
    if (err.code == heif_error_Ok) {
      result = visit(data, size, context);
    }
    free(data);
  }

  free(ids);
  return result;
}

static bool visit_xmp_headroom(const uint8_t *data, size_t size, void *context) {
  double *out = context;
  // iOS 18+/26 writes the authoritative headroom directly into the gain map XMP.
  return xmp_find_double(data, size, "HDRGainMapHeadroom", out);
}

// -- Apple MakerNote --------------------------------------------------------

typedef struct {
  const uint8_t *data;
  size_t size;
  bool big_endian;
} tiff_reader;

static uint16_t tiff_u16(const tiff_reader *r, size_t offset) {
  if (offset + 2 > r->size) {
    return 0;
  }
  const uint8_t *p = r->data + offset;
  return r->big_endian ? (uint16_t)((p[0] << 8) | p[1]) : (uint16_t)((p[1] << 8) | p[0]);
}

static uint32_t tiff_u32(const tiff_reader *r, size_t offset) {
  if (offset + 4 > r->size) {
    return 0;
  }
  const uint8_t *p = r->data + offset;
  if (r->big_endian) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
  }
  return ((uint32_t)p[3] << 24) | ((uint32_t)p[2] << 16) | ((uint32_t)p[1] << 8) | p[0];
}

// Locate the value offset of `tag` within the IFD starting at `ifd_offset`.
static bool tiff_find_tag(const tiff_reader *r, size_t ifd_offset, uint16_t tag, uint16_t *out_type,
                          uint32_t *out_count, uint32_t *out_value_offset) {
  if (ifd_offset + 2 > r->size) {
    return false;
  }
  const uint16_t entries = tiff_u16(r, ifd_offset);
  if (entries > 512) {
    return false;
  }

  for (uint16_t i = 0; i < entries; i++) {
    const size_t entry = ifd_offset + 2 + (size_t)i * 12;
    if (entry + 12 > r->size) {
      return false;
    }
    if (tiff_u16(r, entry) != tag) {
      continue;
    }
    *out_type = tiff_u16(r, entry + 2);
    *out_count = tiff_u32(r, entry + 4);
    *out_value_offset = tiff_u32(r, entry + 8);

    // Values of 4 bytes or fewer are stored inline in the entry itself.
    static const size_t type_sizes[] = {0, 1, 1, 2, 4, 8, 1, 1, 2, 4, 8, 4, 8};
    const size_t unit = (*out_type < sizeof(type_sizes) / sizeof(type_sizes[0]))
                            ? type_sizes[*out_type]
                            : 0;
    if (unit * (size_t)*out_count <= 4) {
      *out_value_offset = (uint32_t)(entry + 8);
    }
    return true;
  }
  return false;
}

static bool tiff_read_srational(const tiff_reader *r, size_t offset, double *out) {
  if (offset + 8 > r->size) {
    return false;
  }
  const int32_t num = (int32_t)tiff_u32(r, offset);
  const int32_t den = (int32_t)tiff_u32(r, offset + 4);
  if (den == 0) {
    return false;
  }
  *out = (double)num / (double)den;
  return true;
}

typedef struct {
  double headroom;
  bool found;
} maker_note_result;

// Apple's legacy HDR headroom encoding. The mapping from MakerNote 0x0021
// (HDRHeadroom) and 0x0030 (HDRGain) to stops of headroom is the well-known
// piecewise fit used by the reference implementations; it matches the explicit
// XMP HDRGainMapHeadroom value to ~0.1% on files that carry both.
static double apple_headroom_from_maker_note(double maker33, double maker48) {
  double stops;
  if (maker33 < 1.0) {
    stops = (maker48 <= 0.01) ? (-20.0 * maker48 + 1.8) : (-0.101 * maker48 + 1.601);
  } else {
    stops = (maker48 <= 0.01) ? (-70.0 * maker48 + 3.0) : (-0.303 * maker48 + 2.303);
  }
  if (stops < 0.0) {
    stops = 0.0;
  }
  return pow(2.0, stops);
}

static bool visit_exif_maker_note(const uint8_t *data, size_t size, void *context) {
  maker_note_result *result = context;

  // HEIF EXIF payloads begin with a 4-byte big-endian offset to the TIFF header.
  if (size < 8) {
    return false;
  }
  const uint32_t tiff_offset = read_be32(data) + 4u;
  if (tiff_offset + 8 > size) {
    return false;
  }

  tiff_reader r = {.data = data + tiff_offset, .size = size - tiff_offset};
  if (memcmp(r.data, "II", 2) == 0) {
    r.big_endian = false;
  } else if (memcmp(r.data, "MM", 2) == 0) {
    r.big_endian = true;
  } else {
    return false;
  }

  const uint32_t ifd0 = tiff_u32(&r, 4);
  uint16_t type = 0;
  uint32_t count = 0;
  uint32_t value = 0;

  if (!tiff_find_tag(&r, ifd0, 0x8769, &type, &count, &value)) {
    return false; // no Exif sub-IFD
  }
  const uint32_t exif_ifd = tiff_u32(&r, value);

  if (!tiff_find_tag(&r, exif_ifd, 0x927c, &type, &count, &value)) {
    return false; // no MakerNote
  }

  // Apple MakerNote: "Apple iOS\0" + 2 bytes + "MM", then a TIFF IFD whose
  // value offsets are relative to the start of the MakerNote blob.
  if ((size_t)value + 14 > r.size || count < 14) {
    return false;
  }
  const uint8_t *maker = r.data + value;
  if (memcmp(maker, "Apple iOS", 9) != 0) {
    return false;
  }

  tiff_reader mr = {.data = maker, .size = count, .big_endian = memcmp(maker + 12, "MM", 2) == 0};
  if (mr.size > r.size - value) {
    mr.size = r.size - value;
  }

  double maker33 = 0.0;
  double maker48 = 0.0;
  bool have33 = false;
  bool have48 = false;

  if (tiff_find_tag(&mr, 14, 0x0021, &type, &count, &value)) {
    have33 = tiff_read_srational(&mr, value, &maker33);
  }
  if (tiff_find_tag(&mr, 14, 0x0030, &type, &count, &value)) {
    have48 = tiff_read_srational(&mr, value, &maker48);
  }

  if (!have33 && !have48) {
    return false;
  }

  result->headroom = apple_headroom_from_maker_note(maker33, maker48);
  result->found = true;
  return true;
}

// ---------------------------------------------------------------------------
// Auxiliary image lookup
// ---------------------------------------------------------------------------

// Find the Apple gain map auxiliary image. Returns NULL when absent.
static struct heif_image_handle *find_gain_map(const struct heif_image_handle *primary, char *out_type,
                                        size_t out_type_size) {
  const int count = heif_image_handle_get_number_of_auxiliary_images(primary, 0);
  if (count <= 0) {
    return NULL;
  }

  heif_item_id *ids = calloc((size_t)count, sizeof(heif_item_id));
  if (ids == NULL) {
    return NULL;
  }
  const int found = heif_image_handle_get_list_of_auxiliary_image_IDs(primary, 0, ids, count);

  struct heif_image_handle *result = NULL;
  for (int i = 0; i < found && result == NULL; i++) {
    struct heif_image_handle *aux = NULL;
    if (heif_image_handle_get_auxiliary_image_handle(primary, ids[i], &aux).code != heif_error_Ok) {
      continue;
    }

    const char *type = NULL;
    if (heif_image_handle_get_auxiliary_type(aux, &type).code == heif_error_Ok && type != NULL) {
      // Match Apple's documented URN, but stay tolerant of future variants that
      // keep the same suffix.
      if (strcmp(type, APPLE_GAIN_MAP_AUX) == 0 || strstr(type, "hdrgainmap") != NULL) {
        if (out_type != NULL) {
          snprintf(out_type, out_type_size, "%s", type);
        }
        result = aux;
      }
      heif_image_handle_release_auxiliary_type(aux, &type);
    }

    if (result != aux) {
      heif_image_handle_release(aux);
    }
  }

  free(ids);
  return result;
}

// ---------------------------------------------------------------------------
// Probe
// ---------------------------------------------------------------------------

typedef struct {
  struct heif_context *ctx;
  struct heif_image_handle *primary;
} heif_session;

static void session_close(heif_session *s) {
  if (s->primary != NULL) {
    heif_image_handle_release(s->primary);
    s->primary = NULL;
  }
  if (s->ctx != NULL) {
    heif_context_free(s->ctx);
    s->ctx = NULL;
  }
}

static bool session_open(const char *path, heif_session *s, char *error) {
  memset(s, 0, sizeof(*s));

  s->ctx = heif_context_alloc();
  if (s->ctx == NULL) {
    set_error(error, "failed to allocate libheif context");
    return false;
  }

  struct heif_error err = heif_context_read_from_file(s->ctx, path, NULL);
  if (err.code != heif_error_Ok) {
    set_error(error, "libheif could not read '%s': %s", path, err.message);
    session_close(s);
    return false;
  }

  err = heif_context_get_primary_image_handle(s->ctx, &s->primary);
  if (err.code != heif_error_Ok) {
    set_error(error, "libheif could not open the primary image: %s", err.message);
    session_close(s);
    return false;
  }

  return true;
}

// Read the ICC profile attached to a handle, if any.
static uint8_t *read_icc(const struct heif_image_handle *handle, size_t *out_size) {
  *out_size = 0;
  if (heif_image_handle_get_color_profile_type(handle) != heif_color_profile_type_prof &&
      heif_image_handle_get_color_profile_type(handle) != heif_color_profile_type_rICC) {
    return NULL;
  }

  const size_t size = heif_image_handle_get_raw_color_profile_size(handle);
  if (size == 0 || size > (16u << 20)) {
    return NULL;
  }

  uint8_t *icc = malloc(size);
  if (icc == NULL) {
    return NULL;
  }
  if (heif_image_handle_get_raw_color_profile(handle, icc).code != heif_error_Ok) {
    free(icc);
    return NULL;
  }

  *out_size = size;
  return icc;
}

static void classify_colour(const struct heif_image_handle *handle, hdr_probe_result *out) {
  size_t icc_size = 0;
  uint8_t *icc = read_icc(handle, &icc_size);
  if (icc != NULL) {
    out->primaries = primaries_from_icc(icc, icc_size);
    free(icc);
  }

  // An nclx profile, when present, tells us the transfer function directly.
  struct heif_color_profile_nclx *nclx = NULL;
  if (heif_image_handle_get_nclx_color_profile(handle, &nclx).code == heif_error_Ok &&
      nclx != NULL) {
    switch (nclx->transfer_characteristics) {
      case 16:
        out->transfer = HDR_TRANSFER_PQ;
        break;
      case 18:
        out->transfer = HDR_TRANSFER_HLG;
        break;
      default:
        break;
    }
    if (out->primaries == HDR_PRIMARIES_UNKNOWN) {
      switch (nclx->color_primaries) {
        case 9:
          out->primaries = HDR_PRIMARIES_BT2020;
          break;
        case 12:
          out->primaries = HDR_PRIMARIES_DISPLAY_P3;
          break;
        case 1:
          out->primaries = HDR_PRIMARIES_SRGB;
          break;
        default:
          break;
      }
    }
    heif_nclx_color_profile_free(nclx);
  }

  if (out->primaries == HDR_PRIMARIES_UNKNOWN) {
    out->primaries = HDR_PRIMARIES_SRGB;
  }
  if (out->transfer == HDR_TRANSFER_UNKNOWN) {
    out->transfer = HDR_TRANSFER_SRGB;
  }
}

bool hdr_probe_file(const char *path, hdr_probe_result *out, char *error) {
  memset(out, 0, sizeof(*out));
  out->gain_map_headroom = 1.0;
  out->headroom_source = "none";

  bool is_heif = false;
  out->has_tmap_brand = file_has_brand(path, "tmap", &is_heif);
  out->is_heif = is_heif;
  if (!is_heif) {
    set_error(error, "not a HEIF/ISOBMFF file");
    return false;
  }

  heif_session session;
  if (!session_open(path, &session, error)) {
    return false;
  }

  out->width = (uint32_t)heif_image_handle_get_width(session.primary);
  out->height = (uint32_t)heif_image_handle_get_height(session.primary);
  out->bit_depth = heif_image_handle_get_luma_bits_per_pixel(session.primary);
  if (out->bit_depth <= 0) {
    out->bit_depth = 8;
  }

  classify_colour(session.primary, out);

  struct heif_image_handle *gain_map =
      find_gain_map(session.primary, out->aux_gain_map_type, sizeof(out->aux_gain_map_type));
  out->has_gain_map = gain_map != NULL;

  if (gain_map != NULL) {
    // The `tmap` derived item pairs base + gain map. libheif does not surface it
    // directly, but its presence is advertised in the ftyp brand list.
    out->has_tmap_item = out->has_tmap_brand;

    double headroom = 0.0;
    // Preferred: explicit XMP headroom, attached to the gain map (iOS 18+/26).
    if (visit_metadata(gain_map, "mime", visit_xmp_headroom, &headroom) && headroom > 0.0) {
      out->gain_map_headroom = headroom;
      out->headroom_source = "xmp";
    } else if (visit_metadata(session.primary, "mime", visit_xmp_headroom, &headroom) &&
               headroom > 0.0) {
      out->gain_map_headroom = headroom;
      out->headroom_source = "xmp";
    } else {
      // Fallback: derive from the Apple MakerNote (older iPhones).
      maker_note_result maker = {.headroom = 0.0, .found = false};
      if (visit_metadata(session.primary, "Exif", visit_exif_maker_note, &maker) && maker.found) {
        out->gain_map_headroom = maker.headroom;
        out->headroom_source = "maker-note";
      }
    }

    heif_image_handle_release(gain_map);

    // A headroom at or below 1.0 means the gain map adds nothing; treat the
    // asset as SDR so it stays on the cheaper Sharp path.
    if (out->gain_map_headroom > 1.01) {
      out->hdr = true;
      out->mode = HDR_MODE_APPLE_GAIN_MAP;
    }
  }

  // Natively HDR-encoded items need no gain map.
  if (!out->hdr && out->bit_depth > 8 &&
      (out->transfer == HDR_TRANSFER_PQ || out->transfer == HDR_TRANSFER_HLG)) {
    out->hdr = true;
    out->mode = HDR_MODE_NATIVE_HDR;
    out->headroom_source = "transfer-characteristics";
  }

  session_close(&session);
  return true;
}

// ---------------------------------------------------------------------------
// Decode + reconstruct
// ---------------------------------------------------------------------------

typedef struct {
  struct heif_image *image;
  const uint8_t *plane;
  size_t stride;
  uint32_t width;
  uint32_t height;
  int channels;
  double max_value;
  bool sixteen_bit;
} decoded_plane;

static void decoded_plane_free(decoded_plane *p) {
  if (p->image != NULL) {
    heif_image_release(p->image);
    p->image = NULL;
  }
}

static bool decode_handle(const struct heif_image_handle *handle, int bit_depth, decoded_plane *out,
                          char *error) {
  memset(out, 0, sizeof(*out));

  const bool wide = bit_depth > 8;
  const enum heif_chroma chroma =
      wide ? heif_chroma_interleaved_RRGGBB_LE : heif_chroma_interleaved_RGB;

  const struct heif_error err =
      heif_decode_image(handle, &out->image, heif_colorspace_RGB, chroma, NULL);
  if (err.code != heif_error_Ok) {
    set_error(error, "libheif decode failed: %s", err.message);
    return false;
  }

  int stride = 0;
  out->plane = heif_image_get_plane_readonly(out->image, heif_channel_interleaved, &stride);
  if (out->plane == NULL || stride <= 0) {
    set_error(error, "libheif returned an empty image plane");
    decoded_plane_free(out);
    return false;
  }

  out->stride = (size_t)stride;
  out->width = (uint32_t)heif_image_get_width(out->image, heif_channel_interleaved);
  out->height = (uint32_t)heif_image_get_height(out->image, heif_channel_interleaved);
  out->channels = 3;
  out->sixteen_bit = wide;
  out->max_value = wide ? (double)((1 << bit_depth) - 1) : 255.0;

  if (out->width == 0 || out->height == 0) {
    set_error(error, "decoded image has zero extent");
    decoded_plane_free(out);
    return false;
  }

  return true;
}

static double sample_channel(const decoded_plane *p, uint32_t x, uint32_t y, int channel) {
  const uint8_t *row = p->plane + (size_t)y * p->stride;
  if (p->sixteen_bit) {
    const uint16_t *pixels = (const uint16_t *)(const void *)row;
    return (double)pixels[(size_t)x * 3 + channel] / p->max_value;
  }
  return (double)row[(size_t)x * 3 + channel] / p->max_value;
}

// Convert one source sample to relative linear light (1.0 == diffuse SDR white).
static void linearise(const hdr_probe_result *probe, double sdr_white_nits, double rgb[3]) {
  switch (probe->transfer) {
    case HDR_TRANSFER_PQ: {
      // PQ is absolute; rescale so SDR white lands on 1.0.
      const double scale = 10000.0 / sdr_white_nits;
      for (int c = 0; c < 3; c++) {
        rgb[c] = hdr_pq_eotf(rgb[c]) * scale;
      }
      break;
    }
    case HDR_TRANSFER_HLG: {
      // Scene light, then the BT.2100 OOTF at a 1000 nit nominal peak.
      double scene[3];
      for (int c = 0; c < 3; c++) {
        scene[c] = hdr_hlg_oetf_inverse(rgb[c]);
      }
      const double luma = 0.2627 * scene[0] + 0.6780 * scene[1] + 0.0593 * scene[2];
      const double gain = (luma > 0.0) ? pow(luma, 0.2) : 0.0;
      const double scale = 1000.0 / sdr_white_nits;
      for (int c = 0; c < 3; c++) {
        rgb[c] = scene[c] * gain * scale;
      }
      break;
    }
    default:
      for (int c = 0; c < 3; c++) {
        rgb[c] = hdr_srgb_eotf(rgb[c]);
      }
      break;
  }
}

bool hdr_decode_linear(const char *path, const hdr_probe_result *probe,
                       const hdr_convert_options *options, hdr_linear_image *out, char *error) {
  memset(out, 0, sizeof(*out));

  heif_session session;
  if (!session_open(path, &session, error)) {
    return false;
  }

  decoded_plane base = {0};
  if (!decode_handle(session.primary, probe->bit_depth, &base, error)) {
    session_close(&session);
    return false;
  }

  // Decode the gain map when we are taking the Apple route.
  decoded_plane gain = {0};
  bool have_gain = false;
  if (probe->mode == HDR_MODE_APPLE_GAIN_MAP) {
    struct heif_image_handle *gain_handle = find_gain_map(session.primary, NULL, 0);
    if (gain_handle != NULL) {
      have_gain = decode_handle(gain_handle, 8, &gain, error);
      heif_image_handle_release(gain_handle);
    }
    if (!have_gain) {
      set_error(error, "gain map was advertised but could not be decoded");
      decoded_plane_free(&base);
      session_close(&session);
      return false;
    }
  }

  // Work out the preview size, preserving aspect ratio.
  uint32_t out_w = base.width;
  uint32_t out_h = base.height;
  if (options->max_size > 0 && (out_w > options->max_size || out_h > options->max_size)) {
    const double scale = (double)options->max_size / (double)(out_w > out_h ? out_w : out_h);
    out_w = (uint32_t)(base.width * scale + 0.5);
    out_h = (uint32_t)(base.height * scale + 0.5);
  }
  if (out_w < 1) {
    out_w = 1;
  }
  if (out_h < 1) {
    out_h = 1;
  }

  float *pixels = calloc((size_t)out_w * out_h * 3, sizeof(float));
  if (pixels == NULL) {
    set_error(error, "out of memory allocating %ux%u output", out_w, out_h);
    decoded_plane_free(&base);
    decoded_plane_free(&gain);
    session_close(&session);
    return false;
  }

  const double headroom = probe->gain_map_headroom;

  // Box-average in linear light. Averaging after linearisation (and after the
  // gain map has been applied) is what keeps highlights from being crushed.
  for (uint32_t oy = 0; oy < out_h; oy++) {
    const uint32_t sy0 = (uint32_t)((uint64_t)oy * base.height / out_h);
    uint32_t sy1 = (uint32_t)((uint64_t)(oy + 1) * base.height / out_h);
    if (sy1 <= sy0) {
      sy1 = sy0 + 1;
    }
    if (sy1 > base.height) {
      sy1 = base.height;
    }

    for (uint32_t ox = 0; ox < out_w; ox++) {
      const uint32_t sx0 = (uint32_t)((uint64_t)ox * base.width / out_w);
      uint32_t sx1 = (uint32_t)((uint64_t)(ox + 1) * base.width / out_w);
      if (sx1 <= sx0) {
        sx1 = sx0 + 1;
      }
      if (sx1 > base.width) {
        sx1 = base.width;
      }

      double accum[3] = {0.0, 0.0, 0.0};
      uint32_t samples = 0;

      for (uint32_t sy = sy0; sy < sy1; sy++) {
        for (uint32_t sx = sx0; sx < sx1; sx++) {
          double rgb[3] = {
              sample_channel(&base, sx, sy, 0),
              sample_channel(&base, sx, sy, 1),
              sample_channel(&base, sx, sy, 2),
          };
          linearise(probe, options->sdr_white_nits, rgb);

          if (have_gain) {
            // The gain map is typically half resolution; map by relative
            // position so any ratio works.
            uint32_t gx = (uint32_t)((uint64_t)sx * gain.width / base.width);
            uint32_t gy = (uint32_t)((uint64_t)sy * gain.height / base.height);
            if (gx >= gain.width) {
              gx = gain.width - 1;
            }
            if (gy >= gain.height) {
              gy = gain.height - 1;
            }

            // Apple's gain map is monochrome; libheif hands it back as RGB.
            const double g = sample_channel(&gain, gx, gy, 0);
            // Apply in log space: out = in * headroom^gain. Apple (and ISO
            // 21496-1) interpolate the gain exponentially between 1x and the
            // headroom, not linearly. Using `1 + (H-1)*g` instead measurably
            // over-brightens midtones and shadows, because H^g is convex.
            const double multiplier = pow(headroom, g);
            for (int c = 0; c < 3; c++) {
              rgb[c] *= multiplier;
            }
          }

          accum[0] += rgb[0];
          accum[1] += rgb[1];
          accum[2] += rgb[2];
          samples++;
        }
      }

      if (samples == 0) {
        samples = 1;
      }
      float *dst = pixels + (((size_t)oy * out_w) + ox) * 3;
      for (int c = 0; c < 3; c++) {
        dst[c] = (float)(accum[c] / samples);
      }
    }
  }

  decoded_plane_free(&base);
  decoded_plane_free(&gain);
  session_close(&session);

  out->pixels = pixels;
  out->width = out_w;
  out->height = out_h;
  out->primaries = probe->primaries;
  return true;
}
