#!/usr/bin/env bash
# Try the HDR pipeline against a folder of photos, without touching the originals.
#
#   ./try-hdr.sh ~/Pictures/SomeFolder [output-dir]
#
# Prints one line per file showing whether it was detected as HDR, and writes an
# HDR AVIF preview for the ones that were.

set -uo pipefail

BIN="${IMMICH_HDR_HEIF_BIN:-$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/build/immich-hdr-heif}"
SRC_DIR="${1:-}"
OUT_DIR="${2:-./hdr-out}"

if [[ -z "$SRC_DIR" || ! -d "$SRC_DIR" ]]; then
  echo "usage: $0 <folder-of-photos> [output-dir]" >&2
  exit 1
fi

if [[ ! -x "$BIN" ]]; then
  echo "helper not built; run 'make' in $(dirname "$BIN")/.." >&2
  exit 1
fi

mkdir -p "$OUT_DIR"

total=0
hdr=0
sdr=0
failed=0

# -print0/read -d handles spaces in filenames.
while IFS= read -r -d '' file; do
  total=$((total + 1))
  name=$(basename "$file")

  probe=$("$BIN" probe "$file" 2>/dev/null)
  if [[ -z "$probe" ]] || ! grep -q '"hdr":true' <<<"$probe"; then
    sdr=$((sdr + 1))
    printf '  %-44s SDR  (unchanged, uses the existing preview)\n' "${name:0:44}"
    continue
  fi

  headroom=$(sed -n 's/.*"gainMapHeadroom":\([0-9.]*\).*/\1/p' <<<"$probe")
  out="$OUT_DIR/${name%.*}-hdr.avif"

  if result=$("$BIN" convert "$file" "$out" --size 1440 --quality 65 2>/dev/null); then
    hdr=$((hdr + 1))
    cll=$(sed -n 's/.*"maxCll":\([0-9]*\).*/\1/p' <<<"$result")
    size=$(( $(stat -f%z "$out" 2>/dev/null || stat -c%s "$out") / 1024 ))
    printf '  %-44s HDR  headroom=%-6s peak=%-4s nits  %s KB\n' "${name:0:44}" "$headroom" "$cll" "$size"
  else
    failed=$((failed + 1))
    printf '  %-44s HDR detected but conversion failed\n' "${name:0:44}"
  fi
done < <(find "$SRC_DIR" -type f \( -iname '*.heic' -o -iname '*.heif' -o -iname '*.hif' \) -print0)

echo
echo "  $total scanned: $hdr HDR, $sdr SDR, $failed failed"
[[ $hdr -gt 0 ]] && echo "  previews written to $OUT_DIR"
exit 0
