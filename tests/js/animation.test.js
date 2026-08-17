import assert from 'node:assert/strict';
import fs from 'node:fs';
import test from 'node:test';

import { parseAnimations } from '../../web/dist/assets/anims.js';
import { parseModel } from '../../web/dist/assets/model.js';
import { PoseEvaluator, findAction, resolveAction, sampleTrack, skinPositions } from '../../web/dist/animation/pose.js';
import { transformBindPose } from '../../web/dist/renderer/static-pose.js';

function arrayBuffer(path) {
  const bytes = fs.readFileSync(path);
  return bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.byteLength);
}

function track(interpolation, outTangent = 0, inTangent = 0) {
  return {
    startFrame: 0,
    frames: new Float32Array([0, 10]), values: new Float32Array([2, 8]),
    inTangents: new Float32Array([0, inTangent]), outTangents: new Float32Array([outTangent, 0]),
    interpolation: new Uint8Array([interpolation, interpolation]),
  };
}

test('animation key sampling matches step, linear, Hermite, and tangent guard rules', () => {
  assert.equal(sampleTrack(track(1), 5), 2);
  assert.equal(sampleTrack(track(2), 5), 5);
  assert.equal(sampleTrack(track(3, 0.6, 0.6), 5), 5);
  assert.equal(sampleTrack(track(3, 10_000, -10_000), 5), 5);
  assert.equal(sampleTrack(track(2), -1), 2);
  assert.equal(sampleTrack(track(2), 20), 8);
});

test('GPU bone rows reproduce the C bind-pose rigid and weighted vertex rules', () => {
  const model = parseModel(arrayBuffer('fixtures/cache/falco-2.model'));
  const animations = parseAnimations(arrayBuffer('fixtures/cache/falco-2.anims'));
  const evaluator = new PoseEvaluator(model, animations);
  const fromRows = skinPositions(model, evaluator.evaluateBindPose().boneRows);
  const expected = transformBindPose(model);
  let maximum = 0;
  for (let i = 0; i < expected.length; i++) maximum = Math.max(maximum, Math.abs(fromRows[i] - expected[i]));
  assert.ok(maximum < 1e-5, `maximum bind-pose delta ${maximum}`);
});

test('action resolution and root-motion suppression match the C fallback policy', () => {
  const model = parseModel(arrayBuffer('fixtures/cache/falco-2.model'));
  const animations = parseAnimations(arrayBuffer('fixtures/cache/falco-2.anims'));
  const wait = findAction(animations, 'Wait1');
  assert.equal(wait, 2);
  assert.deepEqual(resolveAction(animations, 0xffffffff), { index: 2, fallback: true });
  assert.deepEqual(resolveAction(animations, 12), { index: 12, fallback: false });
  const evaluator = new PoseEvaluator(model, animations);
  const bind = evaluator.evaluate(0xffffffff, 0).boneRows.slice();
  const dash = evaluator.evaluate(12, 0).boneRows;
  // Bone 1 rigid row Z translation is texel row 2, component W.
  assert.ok(Math.abs(bind[24 + 11] - dash[24 + 11]) < 1e-5);
});
