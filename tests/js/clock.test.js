import assert from 'node:assert/strict';
import test from 'node:test';

import { ReplayClock } from '../../web/dist/replay/clock.js';

test('replay clock advances 600 integer frames from its time anchor', () => {
  const clock = new ReplayClock(-123, 10_000, 8);
  clock.play(1_000);
  assert.equal(clock.sample(11_000).frame, 608);
  assert.equal(clock.sample(11_000 + 1000 / 60 - 0.001).frame, 608);
  assert.equal(clock.sample(11_000 + 1000 / 60).frame, 609);
});

test('ten-minute playback has zero accumulated simulation drift', () => {
  const clock = new ReplayClock(-123, 100_000, 40);
  clock.play(250.25);
  assert.equal(clock.sample(600_250.25).frame, 36_040);
  clock.pause(600_250.25);
  assert.equal(clock.sample(700_000).frame, 36_040);
  clock.seek(100, 700_000);
  clock.play(700_000);
  assert.equal(clock.sample(710_000).frame, 700);
});

test('seek, step, end clamp, and restart are exact', () => {
  const clock = new ReplayClock(-2, 3);
  assert.equal(clock.step(1, 0).frame, -1);
  assert.equal(clock.seek(99, 1).frame, 3);
  assert.equal(clock.play(2).frame, -2);
  assert.equal(clock.sample(2 + 5000 / 60).frame, 3);
  assert.equal(clock.play(100).frame, -2);
});
