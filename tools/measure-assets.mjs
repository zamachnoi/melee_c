import { readFileSync } from 'node:fs';
import { performance } from 'node:perf_hooks';

import { parseModel } from '../web/dist/assets/model.js';
import { parseAnimations } from '../web/dist/assets/anims.js';
import { parseStage } from '../web/dist/assets/stage.js';
import { parseTimeline } from '../web/dist/replay/timeline.js';

function buffer(path) {
  const bytes = readFileSync(path);
  return { bytes, arrayBuffer: bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.byteLength) };
}

function typedBytes(value) {
  if (ArrayBuffer.isView(value)) return value.byteLength;
  if (!value || typeof value !== 'object') return 0;
  return Object.values(value).reduce((sum, child) => sum + typedBytes(child), 0);
}

for (const path of process.argv.slice(2)) {
  const { bytes, arrayBuffer } = buffer(path);
  const parser = path.endsWith('.model') ? parseModel
    : path.endsWith('.anims') ? parseAnimations
    : path.endsWith('.stage') ? parseStage
    : parseTimeline;
  const start = performance.now();
  const parsed = parser(arrayBuffer);
  const parseMilliseconds = performance.now() - start;
  console.log(JSON.stringify({ path, wireBytes: bytes.byteLength,
    decodedTypedArrayBytes: typedBytes(parsed), parseMilliseconds: +parseMilliseconds.toFixed(3) }));
}
