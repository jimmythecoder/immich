import { HdrHeifCliConverter } from 'src/repositories/image-pipeline/hdr-heif.converter.js';
import { getHdrPreviewPath, HdrHeifPipeline, isHeifExtension } from 'src/repositories/image-pipeline/hdr-heif.pipeline.js';
import {
  HdrConvertResult,
  HdrHeifConverter,
  HdrProbeResult,
} from 'src/repositories/image-pipeline/image-pipeline.types';
import { LoggingRepository } from 'src/repositories/logging.repository.js';
import { Mocked } from 'vitest';

const newLoggerMock = () =>
  ({
    setContext: vitest.fn(),
    debug: vitest.fn(),
    verbose: vitest.fn(),
    log: vitest.fn(),
    warn: vitest.fn(),
    error: vitest.fn(),
    fatal: vitest.fn(),
  }) as unknown as Mocked<LoggingRepository>;

const hdrProbe = (overrides: Partial<HdrProbeResult> = {}): HdrProbeResult => ({
  hdr: true,
  mode: 'apple-gain-map',
  width: 5712,
  height: 4284,
  bitDepth: 8,
  hasGainMap: true,
  hasTmap: true,
  gainMapHeadroom: 2.319156,
  headroomSource: 'xmp',
  sourcePrimaries: 'display-p3',
  sourceTransfer: 'srgb',
  auxGainMapType: 'urn:com:apple:photo:2020:aux:hdrgainmap',
  ...overrides,
});

const newConverterMock = (): Mocked<HdrHeifConverter> => ({
  isAvailable: vitest.fn().mockResolvedValue(true),
  probe: vitest.fn().mockResolvedValue(hdrProbe()),
  convert: vitest.fn(),
});

describe('getHdrPreviewPath', () => {
  it('derives the HDR path from the SDR preview path', () => {
    expect(getHdrPreviewPath('/thumbs/ab/cd/asset-preview.jpeg')).toBe('/thumbs/ab/cd/asset-preview-hdr.avif');
  });

  it('handles any preview extension', () => {
    expect(getHdrPreviewPath('/thumbs/x/preview.webp')).toBe('/thumbs/x/preview-hdr.avif');
  });

  it('is deterministic, so the same input always resolves to the same derivative', () => {
    const path = '/thumbs/ab/cd/asset-preview.jpeg';
    expect(getHdrPreviewPath(path)).toBe(getHdrPreviewPath(path));
  });

  it('does not collide with the source preview path', () => {
    const path = '/thumbs/ab/cd/asset-preview.jpeg';
    expect(getHdrPreviewPath(path)).not.toBe(path);
  });
});

describe('isHeifExtension', () => {
  it.each(['IMG_7125.HEIC', 'photo.heic', 'photo.heif', 'photo.hif', 'PHOTO.HeIc'])('accepts %s', (fileName) => {
    expect(isHeifExtension(fileName)).toBe(true);
  });

  it.each(['photo.jpg', 'photo.jpeg', 'photo.png', 'photo.webp', 'video.mp4', 'raw.arw', 'noextension'])(
    'rejects %s',
    (fileName) => {
      expect(isHeifExtension(fileName)).toBe(false);
    },
  );
});

describe(HdrHeifPipeline.name, () => {
  let sut: HdrHeifPipeline;
  let converterMock: Mocked<HdrHeifConverter>;
  let loggerMock: Mocked<LoggingRepository>;

  const heicInput = { path: '/originals/IMG_7125.HEIC', originalFileName: 'IMG_7125.HEIC' };

  beforeEach(() => {
    converterMock = newConverterMock();
    loggerMock = newLoggerMock();
    sut = new HdrHeifPipeline(loggerMock, converterMock);
  });

  describe('supports', () => {
    it('recognises an Apple HDR HEIC', async () => {
      await expect(sut.supports(heicInput)).resolves.toBe(true);
    });

    it.each([
      ['JPEG', 'photo.jpg'],
      ['PNG', 'photo.png'],
      ['WebP', 'photo.webp'],
      ['RAW', 'photo.arw'],
      ['video', 'clip.mp4'],
    ])('leaves %s on the standard pipeline without probing', async (_label, originalFileName) => {
      const result = await sut.supports({ path: `/originals/${originalFileName}`, originalFileName });

      expect(result).toBe(false);
      expect(converterMock.probe).not.toHaveBeenCalled();
    });

    it('leaves an ordinary SDR HEIC on the standard pipeline', async () => {
      converterMock.probe.mockResolvedValue(
        hdrProbe({ hdr: false, mode: 'none', hasGainMap: false, hasTmap: false, gainMapHeadroom: 1 }),
      );

      await expect(sut.supports(heicInput)).resolves.toBe(false);
    });

    it('falls back safely when the helper is unavailable', async () => {
      converterMock.isAvailable.mockResolvedValue(false);

      await expect(sut.supports(heicInput)).resolves.toBe(false);
      expect(converterMock.probe).not.toHaveBeenCalled();
    });

    it('falls back safely when detection is inconclusive', async () => {
      converterMock.probe.mockResolvedValue(null);

      await expect(sut.supports(heicInput)).resolves.toBe(false);
    });

    it('does not throw when the converter rejects', async () => {
      converterMock.isAvailable.mockRejectedValue(new Error('boom'));

      await expect(sut.supports(heicInput)).rejects.toBeInstanceOf(Error);
    });
  });

  describe('generatePreview', () => {
    it('returns HDR metadata describing the generated AVIF', async () => {
      const convertResult: HdrConvertResult = {
        format: 'avif',
        hdr: true,
        width: 1440,
        height: 1080,
        bitDepth: 10,
        mode: 'apple-gain-map',
        colorPrimaries: 'bt2020',
        transferCharacteristics: 'pq',
        matrixCoefficients: 'bt2020-ncl',
        maxCll: 469,
        maxPall: 123,
        gainMapHeadroom: 2.319156,
        sourcePrimaries: 'display-p3',
        sourceTransfer: 'srgb',
        bytes: 105_589,
      };
      converterMock.convert.mockResolvedValue(convertResult);

      const preview = await sut.generatePreview(heicInput, '/thumbs/preview-hdr.avif', { size: 1440, quality: 65 });

      expect(preview).toEqual({
        path: '/thumbs/preview-hdr.avif',
        format: 'avif',
        hdr: true,
        bitDepth: 10,
        width: 1440,
        height: 1080,
        colorPrimaries: 'bt2020',
        transferCharacteristics: 'pq',
        matrixCoefficients: 'bt2020-ncl',
        maxCll: 469,
        maxPall: 123,
        bytes: 105_589,
      });
    });

    it('never writes to the original path', async () => {
      converterMock.convert.mockResolvedValue({} as HdrConvertResult);

      await sut.generatePreview(heicInput, '/thumbs/preview-hdr.avif', { size: 1440, quality: 65 });

      const [input, output] = converterMock.convert.mock.calls[0];
      expect(input).toBe(heicInput.path);
      expect(output).toBe('/thumbs/preview-hdr.avif');
      expect(output).not.toBe(input);
    });

    it('propagates converter failures so the caller can fall back', async () => {
      converterMock.convert.mockRejectedValue(new Error('decode failed'));

      await expect(
        sut.generatePreview(heicInput, '/thumbs/preview-hdr.avif', { size: 1440, quality: 65 }),
      ).rejects.toThrow('decode failed');
    });
  });
});

describe(HdrHeifCliConverter.name, () => {
  it('reports unavailable when the binary does not exist', async () => {
    const sut = new HdrHeifCliConverter('/nonexistent/immich-hdr-heif');

    // An explicit path is trusted, but probing it must still fail safely.
    await expect(sut.probe('/originals/IMG_7125.HEIC')).resolves.toBeNull();
  });

  it('never throws from probe, even for a missing file', async () => {
    const sut = new HdrHeifCliConverter('/nonexistent/immich-hdr-heif');

    await expect(sut.probe('/does/not/exist.heic')).resolves.toBeNull();
  });
});
