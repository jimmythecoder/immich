import { execFile } from 'node:child_process';
import { access, constants } from 'node:fs/promises';
import path from 'node:path';
import { promisify } from 'node:util';
import {
  HdrConvertOptions,
  HdrConvertResult,
  HdrHeifConverter,
  HdrProbeResult,
} from 'src/repositories/image-pipeline/image-pipeline.types';

const execFileAsync = promisify(execFile);

/** Probing only reads container metadata, so it should always be quick. */
const PROBE_TIMEOUT_MS = 15_000;
/** Full decode of a large tiled HEIC plus an AVIF encode. */
const CONVERT_TIMEOUT_MS = 180_000;

const DEFAULT_BINARY_NAME = 'immich-hdr-heif';

/**
 * Locations searched for the helper binary, in order.
 *
 * `IMMICH_HDR_HEIF_BIN` wins so a deployment can point at its own build; the
 * remaining entries cover the Docker image and a local `make` build.
 */
const candidatePaths = (): string[] => {
  const candidates: string[] = [];

  if (process.env.IMMICH_HDR_HEIF_BIN) {
    candidates.push(process.env.IMMICH_HDR_HEIF_BIN);
  }

  candidates.push(
    `/usr/local/bin/${DEFAULT_BINARY_NAME}`,
    `/usr/bin/${DEFAULT_BINARY_NAME}`,
    path.join(process.cwd(), 'native', 'hdr-heif', 'build', DEFAULT_BINARY_NAME),
    path.join(process.cwd(), 'server', 'native', 'hdr-heif', 'build', DEFAULT_BINARY_NAME),
  );

  return candidates;
};

/**
 * Drives the bundled libheif/libavif helper over `child_process`.
 *
 * This is the first implementation of {@link HdrHeifConverter}. It is
 * intentionally the only place that knows the helper is an external process, so
 * a future native-binding or worker-service backend can replace it wholesale.
 */
export class HdrHeifCliConverter implements HdrHeifConverter {
  private binaryPath: string | null = null;
  private resolution: Promise<string | null> | null = null;

  constructor(binaryPath?: string) {
    this.binaryPath = binaryPath ?? null;
  }

  /**
   * Resolve the helper once and cache the outcome, including failure. A missing
   * helper is an expected deployment state, not an error.
   */
  private resolveBinary(): Promise<string | null> {
    if (this.binaryPath) {
      return Promise.resolve(this.binaryPath);
    }

    this.resolution ??= (async () => {
      for (const candidate of candidatePaths()) {
        try {
          await access(candidate, constants.X_OK);
          this.binaryPath = candidate;
          return candidate;
        } catch {
          // try the next candidate
        }
      }
      return null;
    })();

    return this.resolution;
  }

  async isAvailable(): Promise<boolean> {
    return (await this.resolveBinary()) !== null;
  }

  /**
   * Run the helper and parse its JSON result.
   *
   * The helper writes a single JSON object to stdout for both success and
   * failure, and uses a non-zero exit code only to categorise failures, so we
   * parse stdout regardless of exit status.
   */
  private async run<T>(args: string[], timeout: number): Promise<T> {
    const binary = await this.resolveBinary();
    if (!binary) {
      throw new Error('HDR HEIF helper binary is not available');
    }

    let stdout: string;
    try {
      ({ stdout } = await execFileAsync(binary, args, { timeout, maxBuffer: 1024 * 1024 }));
    } catch (error: any) {
      // A non-zero exit still carries a JSON payload describing the reason.
      if (typeof error?.stdout === 'string' && error.stdout.trimStart().startsWith('{')) {
        stdout = error.stdout;
      } else {
        throw new Error(`HDR HEIF helper failed: ${error?.message ?? error}`, { cause: error });
      }
    }

    let parsed: any;
    try {
      parsed = JSON.parse(stdout.trim());
    } catch {
      throw new Error(`HDR HEIF helper returned unparsable output: ${stdout.slice(0, 200)}`);
    }

    if (!parsed?.ok) {
      throw new Error(parsed?.error ?? 'HDR HEIF helper reported an unknown error');
    }

    return parsed as T;
  }

  async probe(input: string): Promise<HdrProbeResult | null> {
    try {
      return await this.run<HdrProbeResult>(['probe', input], PROBE_TIMEOUT_MS);
    } catch {
      // Detection must never be fatal; the caller falls back to the SDR path.
      return null;
    }
  }

  async convert(input: string, output: string, options: HdrConvertOptions): Promise<HdrConvertResult> {
    const args = ['convert', input, output];

    if (options.size !== undefined) {
      args.push('--size', String(options.size));
    }
    if (options.quality !== undefined) {
      args.push('--quality', String(options.quality));
    }
    if (options.speed !== undefined) {
      args.push('--speed', String(options.speed));
    }

    return this.run<HdrConvertResult>(args, CONVERT_TIMEOUT_MS);
  }
}
