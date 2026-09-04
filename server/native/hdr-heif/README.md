# HDR HEIF → AVIF helper

Decodes Apple HDR HEIC/HEIF images and encodes browser-displayable **10-bit
BT.2020/PQ AVIF** previews.

This exists because Apple's HDR photos store an ordinary 8-bit SDR image plus a
separate *gain map*. Standard decoders (including libvips/Sharp) return only the
SDR base, so the HDR highlights are silently discarded. libheif can read both
parts but does not combine them, so this helper does that step itself.

## What it handles

- **Apple gain maps** — `urn:com:apple:photo:2020:aux:hdrgainmap` auxiliary
  images, including files with a `tmap` derived item (iOS 18+/26).
- **Tiled `grid` images** — large iPhone photos are stored as a grid of HEVC
  tiles (a 5712×4284 photo is a 9×5 grid). libheif assembles these correctly.
- **Natively HDR items** — >8-bit images already tagged PQ or HLG.

## Usage

```
immich-hdr-heif probe   <input>                     # detect HDR structures
immich-hdr-heif convert <input> <output.avif> [...] # generate an HDR preview
immich-hdr-heif inspect <input.avif>                # verify a generated file
```

Every subcommand prints one line of JSON to stdout, for both success and
failure, so the caller can always parse stdout. Diagnostics go to stderr.

```console
$ immich-hdr-heif probe IMG_7125.HEIC
{"ok":true,"hdr":true,"mode":"apple-gain-map","width":5712,"height":4284,...}
```

`convert` options: `--size` (longest edge, default 1440), `--quality` (0-100,
default 65), `--speed` (0-10, default 6), `--sdr-white` (nits assigned to
diffuse white, default 203 per BT.2408).

## How the HDR reconstruction works

1. Decode the SDR base image and the gain map with libheif.
2. Linearise the base image using its actual transfer function.
3. Apply the gain map **in log space**: `out = in × headroom^gain`.
   Apple and ISO 21496-1 interpolate the gain exponentially between 1× and the
   headroom. Using linear interpolation (`1 + (headroom-1) × gain`) is a
   common mistake that visibly over-brightens midtones and shadows.
4. Downscale **in linear light**, so highlights are not crushed.
5. Convert from the source primaries to BT.2020 using a matrix derived from
   chromaticity coordinates, then encode as 10-bit PQ.

The headroom is read from the `HDRGainMapHeadroom` XMP value where present, and
otherwise derived from the Apple MakerNote tags `0x0021`/`0x0030`.

Content light level (`MaxCLL`/`MaxPALL`) is measured from the encoded pixels
rather than assumed, and the BT.2020 tag reflects a real colorimetric
conversion — the metadata describes what the file actually contains.

## Building

Requires libheif (≥ 1.17) and libavif (≥ 0.11).

```bash
make check   # verify the dependencies are present
make         # build build/immich-hdr-heif
```

The source deliberately uses the tagged `struct heif_*` type names so it
compiles against both libheif 1.19 (Debian trixie) and 1.23 (current).

In Docker the helper is built in a dedicated stage and copied to
`/usr/local/bin/immich-hdr-heif`. The server locates it via
`IMMICH_HDR_HEIF_BIN`, then `/usr/local/bin`, then a local `make` build.

## Server integration

The helper sits behind the `HdrHeifConverter` interface
(`server/src/repositories/image-pipeline/`), so the current `child_process`
backend can be replaced with native bindings or a worker service without
touching the pipeline or the media service.

HDR previews are **additive**: the ordinary SDR preview is always generated
first, and any HDR failure is logged and swallowed. The HDR file path is derived
deterministically from the SDR preview path
(`…-preview.jpeg` → `…-preview-hdr.avif`), so no database migration is needed.

## Verifying output

`inspect` decodes an AVIF back and measures it, which distinguishes a genuine
HDR image from one that is merely *tagged* as HDR:

```console
$ immich-hdr-heif inspect preview-hdr.avif
{"ok":true,...,"genuinelyHdr":true,"peakNits":571.9,"fractionAboveSdrWhite":0.42}
```

## Validation

The gain-map reconstruction was validated against Apple CoreImage
(`tools/coreimage_reference.swift`, macOS only), which is the authoritative
interpretation of the format. On the development fixture the peak luminance
agrees within ~2%. `tools/compare_linear.mjs` diffs the two renderings per pixel
and reports error banded by luminance.

To run the integration tests, point the suite at an HDR HEIC:

```bash
IMMICH_HDR_TEST_FIXTURE=/path/to/IMG_7125.HEIC pnpm test
```

They are skipped automatically when the fixture or the binary is absent.
