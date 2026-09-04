# Deploying the HDR build to TrueNAS SCALE

Verified end to end on 2026-09-04: the image below was built for `linux/amd64`,
booted as a full stack, and generated a genuine 10-bit HDR AVIF preview from
`IMG_7125.HEIC` through the normal background job.

You need two files:

- `immich-server-hdr-amd64.tar.gz` (620 MB) - the image
- `docker-compose.truenas.yml` - the app definition

---

## 1. Copy the image to the NAS

From the Mac, replace `truenas.local` and the dataset path with yours:

```bash
scp /tmp/hdr-demo/immich-server-hdr-amd64.tar.gz \
    /tmp/hdr-demo/docker-compose.truenas.yml \
    admin@truenas.local:/mnt/tank/tmp/
```

## 2. Load the image on the NAS

SSH in and load it. This is the step that makes the image available to the app.

```bash
ssh admin@truenas.local
sudo docker load -i /mnt/tank/tmp/immich-server-hdr-amd64.tar.gz
sudo docker images | grep immich-server-hdr
```

You should see `immich-server-hdr   latest`. Then sanity-check the helper
inside the image before deploying anything:

```bash
sudo docker run --rm --entrypoint immich-hdr-heif immich-server-hdr:latest --help
```

## 3. Create the datasets

```bash
sudo mkdir -p /mnt/tank/immich/library /mnt/tank/immich/postgres
```

Postgres must live on **local storage**, not an SMB/NFS share.

## 4. Install the app

In the TrueNAS UI: **Apps → Discover Apps → Custom App → Install via YAML**,
then paste the contents of `docker-compose.truenas.yml`.

Before saving, change:

| Setting | Change to |
|---|---|
| `/mnt/tank/immich/library` | your library dataset |
| `/mnt/tank/immich/postgres` | your postgres dataset |
| `CHANGE_ME_TO_A_RANDOM_PASSWORD` | a real password (**both** places) |
| `TZ` | your timezone |

Then open `http://your-nas:2283` and create your admin account.

---

## Checking it works

Upload an HDR iPhone photo, wait a few seconds, then:

```bash
# An HDR asset should produce three files, not two
sudo find /mnt/tank/immich/library/thumbs -name '*preview*' | tail -5
```

A working HDR asset looks like this - note the SDR preview is still there:

```
<id>_preview.jpeg        <- unchanged SDR preview
<id>_preview-hdr.avif    <- new HDR derivative
```

Verify the AVIF is genuinely HDR rather than merely tagged as HDR:

```bash
HDR=$(sudo find /mnt/tank/immich/library/thumbs -name '*preview-hdr.avif' | head -1)
sudo docker run --rm -v "$(dirname $HDR)":/t:ro \
  --entrypoint immich-hdr-heif immich-server-hdr:latest \
  inspect "/t/$(basename $HDR)"
```

Expect `"genuinelyHdr":true` and a `peakNits` well above 203.

Finally, view the photo in Safari on an HDR display. The web client requests
the AVIF automatically via `<picture>`.

To watch HDR generation live:

```bash
sudo docker logs -f immich_server 2>&1 | grep -i 'hdr\|avif'
```

Note these are debug-level messages, so set `IMMICH_LOG_LEVEL=debug` in the
compose environment if you want to see them.

---

## Things worth knowing

**This image does not auto-update.** You are pinned to this build, so you will
not receive upstream Immich releases or security fixes until you rebuild. Fine
for a test; think carefully before making it your permanent instance.

**HDR generation is additive and non-fatal.** The SDR preview is always written
first. If HDR conversion fails the asset keeps its normal preview and the job
still succeeds - you will never get a black or broken thumbnail from this.

**Only HDR HEIC/HEIF files are affected.** JPEG, PNG, RAW, video, and ordinary
SDR HEIC all continue down the existing Sharp path untouched.

**Existing photos are not converted.** Only newly uploaded assets get HDR
previews. To backfill, run **Administration → Jobs → Generate Thumbnails →
All** (this re-encodes your whole library, so expect it to take a while on an
N100).

**Turning it off.** Set `image.hdr.enabled` to false in
**Administration → Settings → Image Settings**, or just redeploy with the
official `ghcr.io/immich-app/immich-server:v3` image - the HDR files are
additive and ignored by the stock server.

**N100 performance.** Conversion is roughly 2-4 seconds per HDR photo on an
N100 (it decodes a 5712x4284 tiled HEVC image, applies the gain map, and
encodes AV1). It only runs for HDR HEICs, and only once per asset. The N100's
QuickSync does not accelerate AV1 *encoding*, so this is CPU-bound - consider
lowering `image.hdr.quality` or the preview size if a bulk import feels slow.

---

## Rebuilding after code changes

On the Mac, from the repo root:

```bash
docker buildx build --platform linux/amd64 -f server/Dockerfile \
  -t immich-server-hdr:latest --load .
docker save immich-server-hdr:latest | gzip -1 > immich-server-hdr-amd64.tar.gz
```

Takes about 4 minutes on an M1 Max with Rosetta enabled in Docker Desktop
(Settings → General → "Use Rosetta for x86_64/amd64 emulation"). Without
Rosetta this runs under QEMU and is dramatically slower.

If the build fails with `You don't have enough free space`, reclaim the Docker
VM disk with `docker builder prune -af`.
