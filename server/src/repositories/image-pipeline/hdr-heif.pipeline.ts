import { Injectable } from '@nestjs/common';
import { extname } from 'node:path';
import { HdrHeifCliConverter } from 'src/repositories/image-pipeline/hdr-heif.converter.js';
import {
  GeneratedPreview,
  HdrHeifConverter,
  HdrProbeResult,
  ImageInput,
  ImagePipeline,
  PreviewOptions,
} from 'src/repositories/image-pipeline/image-pipeline.types';
import { LoggingRepository } from 'src/repositories/logging.repository.js';

/** Container formats that can carry Apple HDR structures. */
const HEIF_EXTENSIONS = new Set(['.heic', '.heif', '.hif']);

/** Suffix appended to the SDR preview path to derive the HDR derivative. */
const HDR_SUFFIX = '-hdr';
const HDR_EXTENSION = '.avif';

export const isHeifExtension = (fileName: string): boolean => HEIF_EXTENSIONS.has(extname(fileName).toLowerCase());

/**
 * Derive the HDR preview path from the existing SDR preview path.
 *
 * Deterministic by design: it lets the HDR derivative be located, served,
 * deleted, and regenerated without a database migration.
 *
 * `/thumbs/ab/cd/asset-preview.jpeg` -> `/thumbs/ab/cd/asset-preview-hdr.avif`
 */
export const getHdrPreviewPath = (previewPath: string): string => {
  const extension = extname(previewPath);
  const base = extension ? previewPath.slice(0, -extension.length) : previewPath;
  return `${base}${HDR_SUFFIX}${HDR_EXTENSION}`;
};

/**
 * Generates 10-bit HDR AVIF previews for Apple HDR HEIC/HEIF assets.
 *
 * This pipeline is additive: it never replaces the SDR preview, and any failure
 * is reported to the caller so the asset can fall back to the ordinary path.
 */
@Injectable()
export class HdrHeifPipeline implements ImagePipeline {
  readonly name = 'hdr-heif';

  constructor(
    private logger: LoggingRepository,
    private converter: HdrHeifConverter = new HdrHeifCliConverter(),
  ) {
    this.logger.setContext(HdrHeifPipeline.name);
  }

  /**
   * Whether this asset is an HDR-capable HEIC/HEIF.
   *
   * Fails safe: any uncertainty resolves to `false`, which keeps the asset on
   * the existing Sharp path.
   */
  async supports(input: ImageInput): Promise<boolean> {
    const probe = await this.probe(input);
    return probe?.hdr === true;
  }

  /**
   * Inspect an asset, returning `null` when it is not an HDR HEIF or when
   * detection could not be completed.
   */
  async probe(input: ImageInput): Promise<HdrProbeResult | null> {
    if (!isHeifExtension(input.originalFileName) && !isHeifExtension(input.path)) {
      return null;
    }

    if (!(await this.converter.isAvailable())) {
      this.logger.debug('HDR HEIF helper is unavailable; using the standard image pipeline');
      return null;
    }

    const probe = await this.converter.probe(input.path);
    if (!probe) {
      this.logger.debug(`HDR detection was inconclusive for ${input.path}; using the standard image pipeline`);
      return null;
    }

    return probe;
  }

  async generatePreview(input: ImageInput, outputPath: string, options: PreviewOptions): Promise<GeneratedPreview> {
    const result = await this.converter.convert(input.path, outputPath, {
      size: options.size,
      quality: options.quality,
    });

    return {
      path: outputPath,
      format: 'avif',
      hdr: true,
      bitDepth: result.bitDepth,
      width: result.width,
      height: result.height,
      colorPrimaries: result.colorPrimaries,
      transferCharacteristics: result.transferCharacteristics,
      matrixCoefficients: result.matrixCoefficients,
      maxCll: result.maxCll,
      maxPall: result.maxPall,
      bytes: result.bytes,
    };
  }
}
