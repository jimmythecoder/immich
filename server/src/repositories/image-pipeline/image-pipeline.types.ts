import { ImageFormat } from 'src/enum.js';

/** The source image a pipeline is asked to work with. */
export interface ImageInput {
  /** Absolute path to the original asset. Never modified. */
  path: string;
  /** Original filename, used for extension-based checks. */
  originalFileName: string;
}

export interface PreviewOptions {
  /** Bound for the longest edge of the generated preview. */
  size: number;
  /** Encoder quality, 1-100. */
  quality: number;
}

/**
Metadata describing a preview a pipeline produced.

This is deliberately richer than the SDR path needs so that HDR previews can
be described accurately without a database migration - the values are derived
on demand from the generated file.
*/
export interface GeneratedPreview {
  path: string;
  format: 'avif' | ImageFormat;
  hdr: boolean;
  bitDepth: number;
  width: number;
  height: number;
  colorPrimaries?: string;
  transferCharacteristics?: string;
  matrixCoefficients?: string;
  /** Peak content light level in nits, when known. */
  maxCll?: number;
  /** Frame-average light level in nits, when known. */
  maxPall?: number;
  bytes?: number;
}

/**
A strategy for turning an original image into a browser-displayable preview.

Implementations must be side-effect free with respect to the original file.
*/
export interface ImagePipeline {
  readonly name: string;

  /**
  Whether this pipeline can, and should, handle the given input.
  
  Must never throw: a pipeline that cannot make a confident decision should
  report `false` so the caller falls back to the default path.
  */
  supports(input: ImageInput): Promise<boolean>;

  generatePreview(input: ImageInput, outputPath: string, options: PreviewOptions): Promise<GeneratedPreview>;
}

/** Result of inspecting a HEIF file for HDR structures. */
export interface HdrProbeResult {
  hdr: boolean;
  /** How HDR would be reconstructed: `apple-gain-map`, `native-hdr`, or `none`. */
  mode: string;
  width: number;
  height: number;
  bitDepth: number;
  hasGainMap: boolean;
  hasTmap: boolean;
  /** Linear multiplier over SDR white; 1.0 means no headroom. */
  gainMapHeadroom: number;
  headroomSource: string;
  sourcePrimaries: string;
  sourceTransfer: string;
  auxGainMapType: string;
}

export interface HdrConvertResult {
  format: 'avif';
  hdr: true;
  width: number;
  height: number;
  bitDepth: number;
  mode: string;
  colorPrimaries: string;
  transferCharacteristics: string;
  matrixCoefficients: string;
  maxCll: number;
  maxPall: number;
  gainMapHeadroom: number;
  sourcePrimaries: string;
  sourceTransfer: string;
  bytes: number;
}

export interface HdrConvertOptions {
  size?: number;
  quality?: number;
  speed?: number;
}

/**
The decoder/encoder backend used by the HDR pipeline.

Kept deliberately narrow so the current child_process implementation can be
swapped for native bindings, libvips, or a dedicated worker service without
touching the pipeline or the media service.
*/
export interface HdrHeifConverter {
  /** Whether the backend is usable in this deployment. Must never throw. */
  isAvailable(): Promise<boolean>;

  /** Inspect a file for HDR structures. Must never throw. */
  probe(path: string): Promise<HdrProbeResult | null>;

  /** Convert to a 10-bit HDR AVIF. Throws on failure. */
  convert(input: string, output: string, options: HdrConvertOptions): Promise<HdrConvertResult>;
}
