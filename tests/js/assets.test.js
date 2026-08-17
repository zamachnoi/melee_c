import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import test from 'node:test';

import { parseModel, ASSET_CACHE_SUBDIR, ASSET_SCHEMA } from '../../web/dist/assets/model.js';
import { parseAnimations } from '../../web/dist/assets/anims.js';
import { parseStage } from '../../web/dist/assets/stage.js';
import { cacheFile } from './cache-path.js';

const arrayBuffer = path => {
  const bytes = readFileSync(path);
  return bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.byteLength);
};

test('schema-4 Falco records match the C asset fixture', () => {
  const model = parseModel(arrayBuffer(cacheFile('falco-2.model')));
  assert.equal(model.schema, 4);
  assert.equal(model.boneCount, 67);
  assert.ok(model.vertexCount > 0);
  const animations = parseAnimations(arrayBuffer(cacheFile('falco-0.anims')));
  const wait = animations.actions.find(action => action.name.includes('ACTION_Wait1_figatree'));
  assert.ok(wait);
  const bone = wait.joints.find(joint => joint.boneIndex === 3);
  const y = bone.tracks.find(track => track.channel === 6);
  const z = bone.tracks.find(track => track.channel === 7);
  assert.ok(Math.abs(y.values[0] - -0.9263916) < 0.0001);
  assert.ok(Math.abs(z.values[0] - -0.0028076172) < 0.0001);
});

test('schema-4 FD stage matches the C asset fixture', () => {
  const stage = parseStage(arrayBuffer(cacheFile('fd.stage')));
  assert.equal(stage.schema, 4);
  assert.equal(stage.sections.length, 10);
  assert.ok(stage.scale > 0);
  assert.ok(stage.cameraPosition[2] > 0);
});

test('asset parsers reject truncation and oversized counts', () => {
  const source = arrayBuffer(cacheFile('falco-2.model'));
  assert.throws(() => parseModel(source.slice(0, 20)), /truncated binary/);
  const oversized = source.slice(0, 32);
  new DataView(oversized).setUint32(8, 0xffffffff, false);
  assert.throws(() => parseModel(oversized), /exceeds/);
});

test('extracted assets live under a schema version directory', () => {
  assert.equal(ASSET_CACHE_SUBDIR, `v${ASSET_SCHEMA}`);
  assert.equal(ASSET_SCHEMA, 4);
});
