import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import test from 'node:test';

import {
  ReplaySceneIndex, stadiumFieldType, fodReplayHeightToWorldTop,
  FOD_LEFT_START, FOD_RIGHT_START,
} from '../../web/dist/replay/scene.js';
import { parseTimeline } from '../../web/dist/replay/timeline.js';

const bytes = readFileSync('build/timeline-golden.bin');
const timeline = parseTimeline(bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.byteLength));

/** FoD's grGroundParam stage scale as read from the stage DAT (griz.stage). */
const FOD_SCALE = 0.75;

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

test('stadium field type ignores jumbotron preview events', () => {
  assert.equal(stadiumFieldType(3, 3, -1), -1);
  assert.equal(stadiumFieldType(4, 3, -1), -1);
  assert.equal(stadiumFieldType(5, 3, -1), 3);
  assert.equal(stadiumFieldType(6, 3, -1), 3);
  assert.equal(stadiumFieldType(0, 3, -1), 3);
  assert.equal(stadiumFieldType(3, 4, 3), 3);
  assert.equal(stadiumFieldType(5, 4, 3), 4);
  assert.equal(stadiumFieldType(0, 5, 4), 5);
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

test('FoD stage seeds spawn heights and ignores a height-0 frame-0 event', () => {
  const scene = new ReplaySceneIndex({
    stageId: 2,
    startFrame: -123,
    frameCount: 400,
    items: [],
    stageEvents: [
      { frame: 0, kind: 1, index: 0, value0: 0, value1: 0 },
      { frame: 10, kind: 1, index: 0, value0: 25, value1: 0 },
    ],
  }, FOD_SCALE);
  const at = (frame) => frame - (-123);
  assert.equal(scene.fodLeft[at(-123)], 16.125);
  assert.equal(scene.fodRight[at(-123)], 22.125);
  assert.equal(scene.fodLeft[at(0)], 16.125);
  assert.equal(scene.fodRight[at(0)], 22.125);
  assert.equal(scene.fodRight[at(10)], fodReplayHeightToWorldTop(25, FOD_SCALE));
  assert.equal(scene.fodLeft[at(10)], 16.125);
});

test('FoD 0x3F spawn records are world tops; motion records are JObj local Y', () => {
  assert.equal(fodReplayHeightToWorldTop(FOD_LEFT_START, FOD_SCALE), 16.125);
  assert.equal(fodReplayHeightToWorldTop(FOD_RIGHT_START, FOD_SCALE), 22.125);
  assert.equal(fodReplayHeightToWorldTop(20, FOD_SCALE), 16.125);
  assert.equal(fodReplayHeightToWorldTop(28, FOD_SCALE), 22.125);
  const moved = fodReplayHeightToWorldTop(26.814117431640625, FOD_SCALE);
  assert.ok(Math.abs(moved - 21.23558807373047) < 1e-5);
  const scene = new ReplaySceneIndex({
    stageId: 2,
    startFrame: -123,
    frameCount: 200,
    items: [],
    stageEvents: [
      { frame: -123, kind: 1, index: 1, value0: 16.125, value1: 0 },
      { frame: 10, kind: 1, index: 1, value0: 26.814117431640625, value1: 0 },
    ],
  }, FOD_SCALE);
  const at = (frame) => frame - (-123);
  assert.equal(scene.fodLeft[at(-123)], 16.125);
  assert.equal(scene.fodLeft[at(10)], moved);
});
