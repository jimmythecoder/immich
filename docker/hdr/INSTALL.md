# Install the HDR Immich build on TrueNAS 25.10 (Goldeye)

Two files are needed, both in this folder:

- `immich-server-hdr-amd64.tar.gz` (620 MB) — the custom image
- `immich-hdr-truenas.yml` — the app definition

This installs a **separate test instance** on port **2284**, with its own
datasets. Your existing Immich on 2283 is not touched.

---

## Step 1 — Copy the image over SMB

In Finder, drag `immich-server-hdr-amd64.tar.gz` onto any SMB share on the NAS.

Note where that share lives on disk. In the TrueNAS UI: **Shares → SMB**, look
at the **Path** column. It will look like `/mnt/tank/something`.

## Step 2 — Load the image

Open **System → Shell** in the TrueNAS UI, then run (using your path from
step 1):

```bash
sudo docker load -i /mnt/tank/YOUR_SHARE/immich-server-hdr-amd64.tar.gz
```

It takes a minute or two. Confirm it worked:

```bash
sudo docker images | grep immich-server-hdr
```

You should see `immich-server-hdr   latest`. Quick sanity check that the HDR
helper is inside the image:

```bash
sudo docker run --rm --entrypoint immich-hdr-heif immich-server-hdr:latest --help
```

## Step 3 — Create the folders

Still in Shell, using your pool name (`ls /mnt` shows it):

```bash
sudo mkdir -p /mnt/tank/immich-hdr/library /mnt/tank/immich-hdr/postgres
```

The postgres folder must be on **local storage** — never an SMB or NFS share.

## Step 4 — Install the app

In the TrueNAS UI:

**Apps → Discover Apps → Custom App → ⋮ (three dots, top right) →
Install via YAML**

Paste the whole contents of `immich-hdr-truenas.yml`, then change four things:

| Find | Replace with |
|---|---|
| `/mnt/tank/immich-hdr/library` | your real path from step 3 |
| `/mnt/tank/immich-hdr/postgres` | your real path from step 3 |
| `CHANGE_ME` (appears **twice**) | the same password in both places |
| `Pacific/Auckland` | your timezone |

Save and let it deploy.

## Step 5 — Use it

Open **http://YOUR-NAS-IP:2284** and create an admin account.

Upload an HDR iPhone photo. Then view it in **Safari on your HDR Mac display** —
it should show noticeably brighter highlights.

---

## Did it work?

```bash
sudo find /mnt/tank/immich-hdr/library/thumbs -name '*preview*' | tail
```

An HDR photo produces **three** files — note the SDR preview is still there:

```
<id>_preview.jpeg        <- normal preview, unchanged
<id>_preview-hdr.avif    <- the new HDR version
```

To prove the AVIF is genuinely HDR rather than just tagged as HDR:

```bash
HDR=$(sudo find /mnt/tank/immich-hdr/library/thumbs -name '*preview-hdr.avif' | head -1)
sudo docker run --rm -v "$(dirname $HDR)":/t:ro \
  --entrypoint immich-hdr-heif immich-server-hdr:latest \
  inspect "/t/$(basename $HDR)"
```

Expect `"genuinelyHdr":true` and `peakNits` well above 203.

---

## If something goes wrong

**App won't deploy / image not found** — the `docker load` in step 2 didn't
take. Re-run `sudo docker images | grep immich-server-hdr`.

**No `-hdr.avif` file appears** — the photo probably isn't HDR. Only Apple HDR
HEIC/HEIF files qualify; ordinary JPEG, PNG and SDR HEIC are unaffected by
design. Check with:

```bash
sudo docker run --rm -v /mnt/tank/YOUR_SHARE:/in:ro \
  --entrypoint immich-hdr-heif immich-server-hdr:latest \
  probe /in/YOUR_PHOTO.HEIC
```

`"hdr":true` means it should generate one.

**Photo looks flat or washed out** — that is expected on an SDR screen. The
browser only picks the HDR version on a display that reports HDR capability.

**Logs:**

```bash
sudo docker logs immich_hdr_server 2>&1 | tail -50
```

---

## Notes

- **Existing photos are not converted.** Only new uploads get HDR previews. To
  backfill later: **Administration → Jobs → Generate Thumbnails → All**.
- **HDR generation can never break a photo.** The normal preview is written
  first; if HDR conversion fails it is logged and skipped.
- **No auto-updates.** This image is pinned to your build, so it will not
  receive upstream Immich releases or security fixes until you rebuild it.
- **RAM.** Running this alongside your existing Immich on an 8 GB N100 is
  tight. If it feels slow, stop the old app while testing.
- **Speed.** Roughly 2–4 seconds per HDR photo on an N100, once per photo.

## Moving your real library across later

Only once you are happy with the test: stop **both** apps first, then point the
`library` path at your real photo dataset. Never let two Immich instances use
the same library or database at the same time.
