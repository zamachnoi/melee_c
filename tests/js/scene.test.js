import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import test from 'node:test';

import { ReplaySceneIndex } from '../../web/dist/replay/scene.js';
import { parseTimeline } from '../../web/dist/replay/timeline.js';

const bytes = readFileSync('build/timeline-golden.bin');
const timeline = parseTimeline(bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.byteLength));

test('scene index finds sparse items without per-frame searches', () => {
  const scene = new ReplaySceneIndex(timeline);
  const before = -1 - timeline.startFrame;
  const current = 0 - timeline.startFrame;
  assert.equal(scene.itemEnds[before] - scene.itemStarts[before], 0);
  assert.equal(scene.itemEnds[current] - scene.itemStarts[current], 1);
  assert.equal(timeline.items[scene.itemStarts[current]].spawnId, 77);
});

test('dynamic stage state persists after sparse events', () => {
  const scene = new ReplaySceneIndex(timeline);
  const current = 0 - timeline.startFrame;
  const next = 1 - timeline.startFrame;
  assert.equal(scene.fodRight[current], 31.5);
  assert.equal(scene.fodRight[next], 31.5);
  assert.ok(Number.isNaN(scene.fodLeft[next]));
  assert.equal(scene.whispyDirection[current], 2);
  assert.equal(scene.whispyDirection[next], 2);
  assert.equal(scene.stageEventEnds[current] - scene.stageEventStarts[current], 2);
});

test('FoD height holds last move and does not invent a frame-0 drop', () => {
  const scene = new ReplaySceneIndex({
    startFrame: -123,
    frameCount: 400,
    items: [],
    stageEvents: [
      { frame: 10, kind: 1, index: 0, value0: 25, value1: 0 },
      { frame: 12, kind: 1, index: 1, value0: 18.5, value1: 0 },
    ],
  });
  const at = (frame) => frame - (-123);
  assert.ok(Number.isNaN(scene.fodRight[at(0)]));
  assert.ok(Number.isNaN(scene.fodLeft[at(0)]));
  assert.equal(scene.fodRight[at(10)], 25);
  assert.equal(scene.fodRight[at(11)], 25);
  assert.ok(Number.isNaN(scene.fodLeft[at(11)]));
  assert.equal(scene.fodLeft[at(12)], 18.5);
  assert.equal(scene.fodRight[at(200)], 25);
  assert.equal(scene.fodLeft[at(200)], 18.5);
});
