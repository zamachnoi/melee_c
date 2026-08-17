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
