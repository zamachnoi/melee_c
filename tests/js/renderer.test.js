import assert from 'node:assert/strict';
import fs from 'node:fs';
import test from 'node:test';

import { parseStage } from '../../web/dist/assets/stage.js';
import {
  applyGameplayCamera, blendGameplayCamera, cameraViewAxes, cameraViewProjection, createCameraState, fitCameraToBounds,
  gameplayCameraTarget, meleeStageCamera, panCamera, screenToWorld, zoomCameraAt,
} from '../../web/dist/renderer/camera.js';
import { isAcceptedFdSection, isStageSection, positionBounds, transformBindPose } from '../../web/dist/renderer/static-pose.js';
import { evaluateStagePose, skinPositions, stageBoneYOffset } from '../../web/dist/animation/pose.js';

function arrayBuffer(path) {
  const bytes = fs.readFileSync(path);
  return bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.byteLength);
}

function project(matrix, x, y, z) {
  const clipX = matrix[0] * x + matrix[4] * y + matrix[8] * z + matrix[12];
  const clipY = matrix[1] * x + matrix[5] * y + matrix[9] * z + matrix[13];
  const clipW = matrix[3] * x + matrix[7] * y + matrix[11] * z + matrix[15];
  return { x: clipX / clipW, y: clipY / clipW };
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

test('stage section filter keeps playable platforms and drops skybox domes', () => {
  const bf = parseStage(arrayBuffer('cache/grnba.stage'));
  const accepted = bf.sections.map((section, index) => isStageSection(section) ? index : null).filter(x => x !== null);
  assert.deepEqual(accepted, [6], 'Battlefield renders only its platform, not the giant background domes');
  const fod = parseStage(arrayBuffer('cache/griz.stage'));
  const fodAccepted = fod.sections.map((section, index) => isStageSection(section) ? index : null).filter(x => x !== null);
  assert.deepEqual(fodAccepted, [2, 3, 4], 'FoD renders platforms and fountain, dropping the background dome');
  const ys = parseStage(arrayBuffer('cache/grst.stage'));
  const ysAccepted = ys.sections.map((section, index) => isStageSection(section) ? index : null).filter(x => x !== null);
  assert.deepEqual(ysAccepted, [2, 3], "Yoshi's Story keeps the main island, not the cubic skybox");
});

function boneWorldY(section, posed, bone) {
  const ys = [];
  for (let vertex = 0; vertex < section.vertexCount; vertex++) {
    if (section.boneIndices[vertex * 4] === bone) ys.push(posed[vertex * 3 + 1]);
  }
  return ys;
}

test('FoD platform pose puts mesh tops at the world-space replay height', () => {
  const fod = parseStage(arrayBuffer('cache/griz.stage'));
  assert.equal(fod.scale, 0.75);
  const leftHeight = 16.125;
  const rightHeight = 22.125;
  const offsets = (leftBone, rightBone) => new Map([
    [leftBone, stageBoneYOffset(leftHeight, fod.scale)],
    [rightBone, stageBoneYOffset(rightHeight, fod.scale)],
  ]);
  const check = (section, leftBone, rightBone) => {
    const posed = skinPositions(section, evaluateStagePose(section, offsets(leftBone, rightBone)));
    const left = boneWorldY(section, posed, leftBone);
    const right = boneWorldY(section, posed, rightBone);
    assert.ok(left.length > 0 && right.length > 0);
    assert.ok(Math.abs(Math.max(...left) * fod.scale - leftHeight) < 0.01);
    assert.ok(Math.abs(Math.max(...right) * fod.scale - rightHeight) < 0.01);
  };
  check(fod.sections[2], 2, 3);
  check(fod.sections[3], 1, 2);
});

test('stage pose evaluation can fill a reused bone-row buffer', () => {
  const stage = parseStage(arrayBuffer('fixtures/cache/fd.stage'));
  const section = stage.sections.find(value => value.boneCount > 1) ?? stage.sections[0];
  const rows = evaluateStagePose(section, new Map());
  const again = evaluateStagePose(section, new Map([[0, 4]]), rows);
  assert.equal(again, rows);
  const reset = evaluateStagePose(section, new Map(), rows);
  assert.equal(reset, rows);
  const bind = evaluateStagePose(section, new Map());
  for (let i = 0; i < bind.length; i++) assert.ok(Math.abs(reset[i] - bind[i]) < 1e-5);
});

test('melee camera uses the authored FD stage height, angle, and zoom', () => {
  const stage = parseStage(arrayBuffer('fixtures/cache/fd.stage'));
  const viewport = { width: 960, height: 720 };
  const mapped = meleeStageCamera(viewport, stage);
  assert.equal(mapped.fov, 30);
  assert.equal(mapped.verticalAngle, -2);
  assert.equal(mapped.horizontalAngle, 0);
  assert.equal(mapped.distance, 356);
  assert.equal(mapped.eyeY, 45);
  assert.equal(mapped.eyeZ, 356);
  assert.ok(Math.abs(mapped.centerX) < 1e-6);
  assert.ok(Math.abs(mapped.centerY - 32.568) < 0.01);
  assert.ok(Math.abs(mapped.zoom - 3.774) < 0.01);
  const looking = (verticalAngle, centerY = mapped.centerY) => {
    const distance = mapped.distance;
    return cameraViewProjection({
      ...createCameraState(), ...mapped, verticalAngle, centerY,
      eyeX: mapped.centerX - distance * Math.tan(mapped.horizontalAngle * Math.PI / 180),
      eyeY: centerY - distance * Math.tan(verticalAngle * Math.PI / 180),
      eyeZ: distance,
    }, viewport);
  };
  const tilted = looking(-2);
  const steep = looking(-12);
  const flat = looking(0, 45);
  const originTilted = project(tilted, 0, 0, 0);
  const originFlat = project(flat, 0, 0, 0);
  assert.ok(originTilted.y !== originFlat.y, 'downward tilt must change the projected stage top');
  const back = project(tilted, 0, 0, -20);
  const front = project(tilted, 0, 0, 20);
  assert.ok(back.y > front.y, 'looking down should reveal the far edge of the platform top');
  const steepBack = project(steep, 0, 0, -20);
  const steepFront = project(steep, 0, 0, 20);
  assert.ok(steepBack.y > steepFront.y, 'gameplay downward pitch must keep the far edge higher');
  assert.ok(steepBack.y - steepFront.y > back.y - front.y, 'steeper look-down must increase the top foreshortening');
  const high = project(tilted, 0, 40, 0);
  const low = project(tilted, 0, 0, 0);
  assert.ok(high.y > low.y, 'world up must stay screen up');
});

test('fit camera mode updates deterministically', () => {
  const camera = createCameraState();
  fitCameraToBounds(camera, { width: 960, height: 720 }, -50, 0, 50, 40);
  assert.equal(camera.mode, 'fit');
  assert.equal(camera.centerX, 0);
  assert.equal(camera.centerY, 28);
  assert.equal(camera.zoom, 4);
});

test('gameplay camera follows subjects and keeps a downward melee tilt', () => {
  const one = gameplayCameraTarget([{ x: 40, y: 10, facing: 1 }]);
  const two = gameplayCameraTarget([
    { x: -50, y: 8, facing: 1 },
    { x: 50, y: 8, facing: -1 },
  ]);
  assert.ok(one && two);
  assert.ok(one.verticalAngle < 0);
  assert.ok(two.verticalAngle < 0);
  assert.ok(one.eyeY > one.centerY, 'Camera_80029CF8 places the eye above interest');
  assert.ok(Math.abs(two.centerX) < Math.abs(one.centerX));
  assert.ok(one.distance < two.distance);
  assert.equal(one.fov, 38);
});

test('gameplay camera pans inward toward stage center', () => {
  const viewport = { width: 960, height: 720 };
  const target = gameplayCameraTarget([{ x: 80, y: 10, facing: 1 }]);
  assert.ok(target);
  assert.ok(target.eyeX < target.centerX, 'Camera_80029CF8: eye.x = interest.x + x_off, inward for a right-side subject');
  const camera = applyGameplayCamera(createCameraState(), viewport, target, 'follow');
  assert.equal(camera.eyeX, target.eyeX);
  assert.equal(camera.eyeY, target.eyeY);
  const matrix = cameraViewProjection(camera, viewport);
  const player = project(matrix, 80, 10, 0);
  const origin = project(matrix, 0, 10, 0);
  assert.ok(player.x > origin.x, 'a player on the right must stay on the right of screen');
});

test('gameplay camera interpolates interest slower than the eye', () => {
  const viewport = { width: 960, height: 720 };
  const target = gameplayCameraTarget([{ x: 80, y: 10, facing: 1 }]);
  assert.ok(target);
  const blended = blendGameplayCamera(createCameraState(), viewport, target, 'follow', {
    interest: 0.09, eye: 0.27,
  });
  assert.ok(Math.abs(blended.centerX - target.centerX) > 1, 'one tick must not reach the interest');
  assert.ok(Math.abs(blended.eyeX - target.eyeX) > 1, 'one tick must not reach the eye');
  const snapped = applyGameplayCamera(createCameraState(), viewport, target, 'follow');
  assert.equal(snapped.centerX, target.centerX);
  assert.equal(snapped.eyeX, target.eyeX);
});

test('gameplay camera writes into a caller-supplied target without allocating another', () => {
  const out = {
    centerX: 0, centerY: 0, eyeX: 0, eyeY: 0, eyeZ: 0,
    distance: 0, fov: 0, verticalAngle: 0, horizontalAngle: 0,
  };
  const first = gameplayCameraTarget([{ x: 40, y: 10, facing: 1 }]);
  const reused = gameplayCameraTarget([{ x: 40, y: 10, facing: 1 }], out);
  assert.equal(reused, out);
  assert.ok(first && reused);
  assert.equal(reused.centerX, first.centerX);
  assert.equal(reused.eyeY, first.eyeY);
  assert.equal(reused.fov, 38);
});

test('camera view axes stay orthonormal with a horizontal right vector', () => {
  const camera = createCameraState();
  const { right, up } = cameraViewAxes(camera);
  assert.ok(Math.abs(right[1]) < 1e-6, 'right stays in the XZ plane');
  assert.ok(Math.abs(Math.hypot(...right) - 1) < 1e-6);
  assert.ok(Math.abs(right[0] * up[0] + right[1] * up[1] + right[2] * up[2]) < 1e-6);
});

test('camera view projection can fill a reused matrix buffer', () => {
  const camera = createCameraState();
  const viewport = { width: 960, height: 720 };
  const first = cameraViewProjection(camera, viewport);
  const reused = new Float32Array(16);
  const second = cameraViewProjection(camera, viewport, reused);
  assert.equal(second, reused);
  assert.deepEqual([...reused], [...first]);
  cameraViewProjection(camera, viewport, reused);
  assert.deepEqual([...reused], [...first]);
});

test('free camera pan and pointer-centered zoom preserve coordinates', () => {
  const camera = { ...createCameraState(), centerX: 4, centerY: 9, zoom: 5 };
  const panned = panCamera({ ...camera }, 25, -10);
  assert.equal(panned.mode, 'free');
  assert.equal(panned.centerX, -1);
  assert.equal(panned.centerY, 7);
  const viewport = { width: 960, height: 720 };
  const pointer = { x: 630, y: 270 };
  const before = screenToWorld(camera, viewport, pointer.x, pointer.y);
  const zoomed = zoomCameraAt({ ...camera }, viewport, pointer.x, pointer.y, 1.7);
  const after = screenToWorld(zoomed, viewport, pointer.x, pointer.y);
  assert.ok(Math.abs(before.x - after.x) < 1e-9);
  assert.ok(Math.abs(before.y - after.y) < 1e-9);
  assert.equal(panCamera(camera, 0, 0), camera);
});
