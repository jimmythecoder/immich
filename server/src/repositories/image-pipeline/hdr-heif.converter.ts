import { execFile } from 'node:child_process';
import { access, constants } from 'node:fs/promises';
import path from 'node:path';
import { promisify } from 'node:util';
import {
  HdrConvertOptions,
  HdrConvertResult,
  HdrHeifConverter,
  HdrProbeResult,
} from 'src/repositories/image-pipeline/image-pipeline.types.js';

const execFileAsync = promisify(execFile);

/** Probing only reads container metadata, so it should always be quick. */
const PROBE_TIMEOUT_MS = 15_000;
/** Full decode of a large tiled HEIC plus an AVIF encode. */
const CONVERT_TIMEOUT_MS = 180_000;

const DEFAULT_BINARY_NAME = 'immich-hdr-heif';

/**
Bounds how many conversions may run at once.

A single conversion of a 48MP HEIC peaks around 200MB RSS: the full-size base
image, the gain map, and the linear working buffer all have to be resident at
the same time. Thumbnail generation defaults to a job concurrency of 3, and
Sharp is simultaneously decoding the same large images, so leaving this
unbounded can exhaust memory on small hosts (an 8GB NAS running the whole
stack will OOM part-way through a bulk import).

Serialising by default costs very little - the encoder is already
multi-threaded - and makes memory use predictable. Raise it on larger hosts
with IMMICH_HDR_CONCURRENCY.
*/
const parseConcurrency = (): number => {
  const raw = Number(process.env.IMMICH_HDR_CONCURRENCY);
  if (!Number.isSafeInteger(raw) || raw < 1) {
    return 1;
  }
  return Math.min(raw, 8);
};

/**
Minimal counting semaphore.

On release the slot is handed directly to the next waiter rather than being
decremented and re-acquired, which would let a new caller slip in ahead of
the queue and briefly exceed the limit.
*/
class Semaphore {
  private active = 0;
  private waiting: Array<() => void> = [];

  constructor(private readonly max: number) {}

  async acquire(): Promise<() => void> {
    if (this.active < this.max) {
      this.active++;
    } else {
      await new Promise<void>((resolve) => {
        this.waiting.push(resolve);
      });
    }

    let released = false;
    return () => {
      if (released) {
        return;
      }
      released = true;

      const next = this.waiting.shift();
      if (next) {
        next();
      } else {
        this.active--;
      }
    };
  }
}

// Shared across instances: the constraint is host memory, not any one object.
const conversionLimiter = new Semaphore(parseConcurrency());

/**
Locations searched for the helper binary, in order.

`IMMICH_HDR_HEIF_BIN` wins so a deployment can point at its own build; the
remaining entries cover the Docker image and a local `make` build.
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
Drives the bundled libheif/libavif helper over `child_process`.

This is the first implementation of {@link HdrHeifConverter}. It is
intentionally the only place that knows the helper is an external process, so
a future native-binding or worker-service backend can replace it wholesale.
*/
export class HdrHeifCliConverter implements HdrHeifConverter {
  private binaryPath: string | null = null;
  private resolution: Promise<string | null> | null = null;

  constructor(binaryPath?: string) {
    this.binaryPath = binaryPath ?? null;
  }

  /**
  Resolve the helper once and cache the outcome, including failure. A missing
  helper is an expected deployment state, not an error.
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
  Run the helper and parse its JSON result.
  
  The helper writes a single JSON object to stdout for both success and
  failure, and uses a non-zero exit code only to categorise failures, so we
  parse stdout regardless of exit status.
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

    // Queue rather than run in parallel; see conversionLimiter above.
    const release = await conversionLimiter.acquire();
    try {
      return await this.run<HdrConvertResult>(args, CONVERT_TIMEOUT_MS);
    } finally {
      release();
    }
  }
}
