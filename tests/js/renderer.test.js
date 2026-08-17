import assert from 'node:assert/strict';
import fs from 'node:fs';
import test from 'node:test';

import { parseStage } from '../../web/dist/assets/stage.js';
import { fitCameraToBounds, followCamera, panCamera, screenToWorld, zoomCameraAt } from '../../web/dist/renderer/camera.js';
import { isAcceptedFdSection, positionBounds, transformBindPose } from '../../web/dist/renderer/static-pose.js';

function arrayBuffer(path) {
  const bytes = fs.readFileSync(path);
  return bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.byteLength);
}

test('bind-pose transform reproduces the C FD section visibility rule', () => {
  const stage = parseStage(arrayBuffer('fixtures/cache/fd.stage'));
  assert.deepEqual(stage.sections.map(isAcceptedFdSection), [false, false, false, true, false, false, false, false, false, false]);
  const bounds = positionBounds(transformBindPose(stage.sections[3]));
  assert.ok(Math.abs(bounds[0] - -85.6) < 0.2);
  assert.ok(Math.abs(bounds[1] - -139.3) < 0.2);
  assert.ok(Math.abs(bounds[3] - 85.6) < 0.2);
  assert.ok(Math.abs(bounds[4]) < 0.1);
});

test('fit and follow camera modes update deterministically', () => {
  const camera = { mode: 'melee', centerX: 0, centerY: 0, zoom: 1, targetPort: null, smoothing: 0 };
  fitCameraToBounds(camera, { width: 960, height: 720 }, -50, 0, 50, 40);
  assert.deepEqual(camera, { mode: 'fit', centerX: 0, centerY: 28, zoom: 4, targetPort: null, smoothing: 0 });
  followCamera(camera, { width: 960, height: 720 }, 12, 30, 2);
  assert.deepEqual(camera, { mode: 'follow', centerX: 12, centerY: 45, zoom: 5.5, targetPort: 2, smoothing: 0 });
});

test('free camera pan and pointer-centered zoom preserve coordinates', () => {
  const camera = { mode: 'melee', centerX: 4, centerY: 9, zoom: 5, targetPort: null, smoothing: 0 };
  const panned = panCamera(camera, 25, -10);
  assert.deepEqual(panned, { ...camera, mode: 'free', centerX: -1, centerY: 7 });
  const viewport = { width: 960, height: 720 };
  const pointer = { x: 630, y: 270 };
  const before = screenToWorld(camera, viewport, pointer.x, pointer.y);
  const zoomed = zoomCameraAt(camera, viewport, pointer.x, pointer.y, 1.7);
  const after = screenToWorld(zoomed, viewport, pointer.x, pointer.y);
  assert.ok(Math.abs(before.x - after.x) < 1e-9);
  assert.ok(Math.abs(before.y - after.y) < 1e-9);
});
