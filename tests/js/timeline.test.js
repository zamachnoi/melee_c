import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import test from 'node:test';

import { parseTimeline } from '../../web/dist/replay/timeline.js';

const fixturePath = process.env.TIMELINE_FIXTURE || 'build/timeline-golden.bin';
const arrayBuffer = path => {
  const bytes = readFileSync(path);
  return bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.byteLength);
};

test('browser timeline parser matches the C golden serializer', () => {
  const timeline = parseTimeline(arrayBuffer(fixturePath));
  assert.equal(timeline.startFrame, -1);
  assert.equal(timeline.endFrame, 1);
  assert.equal(timeline.stageId, 32);
  assert.equal(timeline.assetSchema, 4);
  assert.equal(timeline.liveProtocol, 1);
  assert.deepEqual(timeline.players.map(p => p.name), ['Falco Fixture', 'Fox Fixture']);
  assert.equal(timeline.frame(0, -1).x, 1.5);
  assert.equal(timeline.frame(0, 1).flags, 1);
  assert.equal(timeline.frame(1, -1), null);
  assert.equal(timeline.frame(1, 0).character, 11);
  assert.equal(timeline.frame(2, 0).x, 9);
  assert.equal(timeline.items[0].spawnId, 77);
  assert.equal(timeline.items[0].owner, -1);
  assert.deepEqual(timeline.stageEvents.map(e => [e.kind, e.value0]), [[1, 31.5], [2, 2]]);
  assert.deepEqual(Array.from(timeline.camera.zoom), [3, 6, 9]);
});

test('timeline parser rejects truncation and schema drift', () => {
  const good = arrayBuffer(fixturePath);
  assert.throws(() => parseTimeline(good.slice(0, 63)), /truncated header/);
  const wrong = good.slice(0);
  new DataView(wrong).setUint16(4, 99, false);
  assert.throws(() => parseTimeline(wrong), /unsupported schema 99/);
});
