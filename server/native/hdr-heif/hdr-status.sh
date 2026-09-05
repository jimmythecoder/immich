#!/usr/bin/env bash
# Show which assets have an HDR AVIF preview.
#
#   ./hdr-status.sh /mnt/Storage/immich-hdr/library
#
# Counts previews, lists the HDR ones, and reports total space used.

set -uo pipefail

LIB="${1:-/mnt/Storage/immich-hdr/library}"
THUMBS="$LIB/thumbs"

if [[ ! -d "$THUMBS" ]]; then
  echo "no thumbs directory at $THUMBS" >&2
  echo "pass your library path, e.g. $0 /mnt/Storage/immich-hdr/library" >&2
  exit 1
fi

total=$(find "$THUMBS" -name '*_preview.*' 2>/dev/null | wc -l | tr -d ' ')
hdr=$(find "$THUMBS" -name '*_preview-hdr.avif' 2>/dev/null | wc -l | tr -d ' ')

echo "previews:      $total"
echo "with HDR AVIF: $hdr"

if [[ "$hdr" -gt 0 ]]; then
  echo
  echo "HDR assets (newest first):"
  # -print0 keeps this safe if a path ever contains spaces.
  find "$THUMBS" -name '*_preview-hdr.avif' -print0 2>/dev/null |
    xargs -0 ls -lt 2>/dev/null |
    head -20 |
    awk '{ n=split($NF,p,"/"); id=p[n]; sub(/_preview-hdr\.avif$/,"",id);
           printf "  %-40s %8.1f KB\n", id, $5/1024 }'

  echo
  size=$(find "$THUMBS" -name '*_preview-hdr.avif' -exec ls -l {} + 2>/dev/null |
    awk '{s+=$5} END {printf "%.1f", s/1048576}')
  echo "total HDR preview size: ${size:-0} MB"
fi

if [[ "$total" -gt 0 && "$hdr" -eq 0 ]]; then
  echo
  echo "No HDR previews yet. Either none of these photos are HDR HEIC/HEIF,"
  echo "or HDR generation is disabled in Administration -> Settings -> Image."
fi
