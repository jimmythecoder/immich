import { HdrHeifCliConverter } from 'src/repositories/image-pipeline/hdr-heif.converter';

/**
 * The conversion limiter exists to stop bulk imports exhausting host memory:
 * each conversion of a large HEIC peaks around 200MB, and thumbnail generation
 * runs three jobs concurrently by default.
 */
describe('HdrHeifCliConverter concurrency', () => {
  it('serialises conversions so peak memory stays bounded', async () => {
    const sut = new HdrHeifCliConverter('/nonexistent/binary');

    let active = 0;
    let peak = 0;

    // Stand in for the child process, recording how many run at once.
    const run = vitest.fn(async () => {
      active++;
      peak = Math.max(peak, active);
      await new Promise((resolve) => setTimeout(resolve, 5));
      active--;
      return {} as never;
    });
    (sut as unknown as { run: typeof run }).run = run;

    await Promise.all(
      Array.from({ length: 8 }, (_, i) => sut.convert(`/in/${i}.heic`, `/out/${i}.avif`, { size: 1440, quality: 65 })),
    );

    expect(run).toHaveBeenCalledTimes(8);
    expect(peak).toBe(1);
  });

  it('releases the slot when a conversion fails, so one error cannot deadlock the queue', async () => {
    const sut = new HdrHeifCliConverter('/nonexistent/binary');

    let calls = 0;
    (sut as unknown as { run: () => Promise<never> }).run = () => {
      calls++;
      return Promise.reject(new Error('decode failed'));
    };

    for (let i = 0; i < 3; i++) {
      await expect(sut.convert('/in.heic', '/out.avif', { size: 1440, quality: 65 })).rejects.toThrow('decode failed');
    }

    // A leaked slot would hang the third call rather than reject it.
    expect(calls).toBe(3);
  });
});
