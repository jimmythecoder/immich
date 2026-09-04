// Compare the C helper's reconstructed HDR buffer against the CoreImage
// reference. Both files are raw float32 RGB in linear Display P3, with 1.0
// meaning diffuse SDR white.
//
//   node compare_linear.mjs ours.f32 apple.f32 <width> <height>

import { readFileSync } from 'node:fs';

const [, , oursPath, applePath, widthArg, heightArg] = process.argv;
if (!oursPath || !applePath) {
  console.error('usage: node compare_linear.mjs <ours.f32> <apple.f32> <width> <height>');
  process.exit(1);
}

const width = Number(widthArg);
const height = Number(heightArg);
const expected = width * height * 3;

const ours = new Float32Array(readFileSync(oursPath).buffer.slice(0));
const apple = new Float32Array(readFileSync(applePath).buffer.slice(0));

if (ours.length !== expected || apple.length !== expected) {
  console.error(`length mismatch: ours=${ours.length} apple=${apple.length} expected=${expected}`);
  process.exit(1);
}

let sumAbsolute = 0;
let sumSquared = 0;
let sumOurs = 0;
let sumApple = 0;
let peakOurs = 0;
let peakApple = 0;
let maxAbsolute = 0;

// Ratio statistics restricted to meaningful signal, so near-black pixels (where
// a tiny absolute difference is a huge relative one) do not dominate.
const ratios = [];
// Bucket by SDR luminance so we can see whether any error is concentrated in
// shadows, midtones, or highlights, which is what a wrong gain-map curve does.
const buckets = [
  { name: 'shadow   (<0.1)', min: 0, max: 0.1, sumRatio: 0, count: 0 },
  { name: 'midtone  (0.1-0.5)', min: 0.1, max: 0.5, sumRatio: 0, count: 0 },
  { name: 'upper    (0.5-1.0)', min: 0.5, max: 1.0, sumRatio: 0, count: 0 },
  { name: 'highlight(>1.0)', min: 1.0, max: Infinity, sumRatio: 0, count: 0 },
];

for (let i = 0; i < expected; i++) {
  const a = ours[i];
  const b = apple[i];
  const diff = Math.abs(a - b);

  sumAbsolute += diff;
  sumSquared += diff * diff;
  sumOurs += a;
  sumApple += b;
  if (a > peakOurs) peakOurs = a;
  if (b > peakApple) peakApple = b;
  if (diff > maxAbsolute) maxAbsolute = diff;

  if (b > 0.02) {
    const ratio = a / b;
    ratios.push(ratio);
    for (const bucket of buckets) {
      if (b >= bucket.min && b < bucket.max) {
        bucket.sumRatio += ratio;
        bucket.count++;
        break;
      }
    }
  }
}

ratios.sort((x, y) => x - y);
const percentile = (p) => ratios[Math.min(ratios.length - 1, Math.floor(ratios.length * p))];

const meanAbsolute = sumAbsolute / expected;
const rmse = Math.sqrt(sumSquared / expected);
const meanOurs = sumOurs / expected;
const meanApple = sumApple / expected;

console.log('per-pixel comparison (linear Display P3, 1.0 = SDR white)');
console.log(`  samples              ${expected.toLocaleString()}`);
console.log(`  mean (ours / apple)  ${meanOurs.toFixed(5)} / ${meanApple.toFixed(5)}  ` +
  `(${((meanOurs / meanApple - 1) * 100).toFixed(2)}%)`);
console.log(`  peak (ours / apple)  ${peakOurs.toFixed(4)} / ${peakApple.toFixed(4)}  ` +
  `(${((peakOurs / peakApple - 1) * 100).toFixed(2)}%)`);
console.log(`  mean abs error       ${meanAbsolute.toFixed(5)}`);
console.log(`  rmse                 ${rmse.toFixed(5)}`);
console.log(`  max abs error        ${maxAbsolute.toFixed(5)}`);
console.log('  ratio ours/apple');
console.log(`    p01 ${percentile(0.01).toFixed(4)}  p50 ${percentile(0.5).toFixed(4)}  ` +
  `p99 ${percentile(0.99).toFixed(4)}`);
console.log('  mean ratio by luminance band');
for (const bucket of buckets) {
  if (bucket.count === 0) continue;
  const share = ((bucket.count / ratios.length) * 100).toFixed(1);
  console.log(`    ${bucket.name}  ${(bucket.sumRatio / bucket.count).toFixed(4)}  (${share}% of samples)`);
}
