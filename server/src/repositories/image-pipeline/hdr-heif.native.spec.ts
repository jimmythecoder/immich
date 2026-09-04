import { execFile } from 'node:child_process';
import { accessSync, constants as fsConstants } from 'node:fs';
import { access, mkdtemp, rm, stat } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { promisify } from 'node:util';
import { HdrHeifCliConverter } from 'src/repositories/image-pipeline/hdr-heif.converter';

const execFileAsync = promisify(execFile);

/**
 * Integration tests for the native libheif/libavif helper.
 *
 * These exercise the real decoder and encoder against a real Apple HDR HEIC.
 * They are skipped automatically when the helper has not been built or the
 * fixture is not present, so they never block a normal `npm test` run.
 *
 * Point IMMICH_HDR_TEST_FIXTURE at a HDR HEIC to run them, e.g. the file this
 * feature was developed against (IMG_7125.HEIC, iPhone 17 Pro, 5712x4284,
 * tiled 9x5 HEVC grid plus an `hdrgainmap` auxiliary image and a `tmap` item).
 */
const binaryPath =
  process.env.IMMICH_HDR_HEIF_BIN ?? join(process.cwd(), 'native', 'hdr-heif', 'build', 'immich-hdr-heif');
const fixturePath = process.env.IMMICH_HDR_TEST_FIXTURE;

const canRun = () => {
  if (!fixturePath) {
    return false;
  }
  try {
    accessSync(binaryPath, fsConstants.X_OK);
    accessSync(fixturePath, fsConstants.R_OK);
    return true;
  } catch {
    return false;
  }
};

const available = canRun();

describe.skipIf(!available)('HDR HEIF native helper', () => {
  let workDir: string;
  let converter: HdrHeifCliConverter;

  beforeAll(async () => {
    workDir = await mkdtemp(join(tmpdir(), 'immich-hdr-test-'));
    converter = new HdrHeifCliConverter(binaryPath);
  });

  afterAll(async () => {
    await rm(workDir, { recursive: true, force: true });
  });

  // A. Detection
  describe('detection', () => {
    it('recognises the fixture as an HDR HEIC', async () => {
      const probe = await converter.probe(fixturePath!);

      expect(probe).not.toBeNull();
      expect(probe!.hdr).toBe(true);
    });

    it('identifies the Apple gain map structures', async () => {
      const probe = await converter.probe(fixturePath!);

      expect(probe!.mode).toBe('apple-gain-map');
      expect(probe!.hasGainMap).toBe(true);
      expect(probe!.auxGainMapType).toContain('hdrgainmap');
    });

    it('reads a plausible HDR headroom above SDR white', async () => {
      const probe = await converter.probe(fixturePath!);

      expect(probe!.gainMapHeadroom).toBeGreaterThan(1);
      // Anything beyond ~16x would indicate a misparsed value.
      expect(probe!.gainMapHeadroom).toBeLessThan(16);
      expect(['xmp', 'maker-note']).toContain(probe!.headroomSource);
    });

    it('reports the source colour space rather than assuming sRGB', async () => {
      const probe = await converter.probe(fixturePath!);

      expect(probe!.sourcePrimaries).not.toBe('unknown');
    });

    it('returns null for a file that is not HEIF', async () => {
      await expect(converter.probe(join(workDir, 'missing.heic'))).resolves.toBeNull();
    });
  });

  // B. Decode + C. AVIF
  describe('conversion', () => {
    let output: string;
    let result: Awaited<ReturnType<HdrHeifCliConverter['convert']>>;

    beforeAll(async () => {
      output = join(workDir, 'IMG_7125-hdr.avif');
      result = await converter.convert(fixturePath!, output, { size: 1440, quality: 65 });
    }, 240_000);

    it('decodes without failing or producing an empty image', async () => {
      const stats = await stat(output);

      expect(stats.size).toBeGreaterThan(1024);
      expect(result.bytes).toBe(stats.size);
    });

    it('produces a 10-bit AVIF', () => {
      expect(result.format).toBe('avif');
      expect(result.bitDepth).toBe(10);
    });

    it('preserves the aspect ratio and honours the requested size', () => {
      expect(Math.max(result.width, result.height)).toBe(1440);
      // 5712x4284 is 4:3.
      expect(result.width / result.height).toBeCloseTo(4 / 3, 2);
    });

    it('emits HDR colour metadata rather than ordinary 8-bit sRGB', () => {
      expect(result.transferCharacteristics).toBe('pq');
      expect(result.colorPrimaries).toBe('bt2020');
      expect(result.matrixCoefficients).toBe('bt2020-ncl');
    });

    it('records content light levels above SDR white', () => {
      // BT.2408 diffuse white is 203 nits; genuine HDR must exceed it.
      expect(result.maxCll).toBeGreaterThan(203);
      expect(result.maxPall).toBeGreaterThan(0);
      expect(result.maxPall).toBeLessThanOrEqual(result.maxCll);
    });

    it('is genuinely HDR, not merely tagged as HDR', async () => {
      // Decode the output back and measure the actual pixel luminance, which is
      // the only way to distinguish real HDR from an SDR image in a 10-bit
      // container carrying HDR tags.
      const { stdout } = await execFileAsync(binaryPath, ['inspect', output], { timeout: 120_000 });
      const inspection = JSON.parse(stdout.trim());

      expect(inspection.ok).toBe(true);
      expect(inspection.genuinelyHdr).toBe(true);
      expect(inspection.bitDepth).toBe(10);
      expect(inspection.peakNits).toBeGreaterThan(203);
      // A meaningful share of the image should exceed SDR white.
      expect(inspection.fractionAboveSdrWhite).toBeGreaterThan(0.001);
    }, 120_000);

    it('uses precision below 8 bits, proving it is not an upscaled 8-bit image', async () => {
      const { stdout } = await execFileAsync(binaryPath, ['inspect', output], { timeout: 120_000 });

      expect(JSON.parse(stdout.trim()).usesSub8BitPrecision).toBe(true);
    }, 120_000);

    it('leaves the original file untouched', async () => {
      const before = await stat(fixturePath!);
      await converter.convert(fixturePath!, join(workDir, 'again.avif'), { size: 512, quality: 50 });
      const after = await stat(fixturePath!);

      expect(after.size).toBe(before.size);
      expect(after.mtimeMs).toBe(before.mtimeMs);
    }, 240_000);
  });

  // D. Fallback
  describe('failure handling', () => {
    it('rejects rather than writing a broken file when the source is missing', async () => {
      await expect(
        converter.convert(join(workDir, 'nope.heic'), join(workDir, 'nope.avif'), { size: 512, quality: 50 }),
      ).rejects.toThrow();

      await expect(access(join(workDir, 'nope.avif'))).rejects.toThrow();
    });
  });
});
