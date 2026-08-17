import assert from 'node:assert/strict';
import fs from 'node:fs';
import test from 'node:test';

import { parseModel } from '../../web/dist/assets/model.js';
import { transformBindPose } from '../../web/dist/renderer/static-pose.js';
import { cacheFile } from './cache-path.js';
import {
  CHAR_FALCO, CHAR_FOX, createHexPrism, createUnitCylinder, createUvSphere,
  effectAssetUrl, fitEffectScale, isFalcoLaser, isFalcoPhantasm, isFirefoxAction,
  isFoxIllusion, isFoxLaser, isShieldState, isShineAction,
  isSpacie, laserLength, parseEffectCatalog, portShieldColor, shieldRadius,
  SHIELD_RADIUS_AT_FULL, xyExtent, xyRadius,
} from '../../web/dist/renderer/effects.js';

test('shield radius scales with remaining shield size', () => {
  assert.equal(shieldRadius(60), SHIELD_RADIUS_AT_FULL);
  assert.equal(shieldRadius(30), SHIELD_RADIUS_AT_FULL / 2);
  assert.equal(shieldRadius(0), 2.5);
  assert.equal(shieldRadius(120), SHIELD_RADIUS_AT_FULL);
});

test('shield state matches GuardOn through GuardReflect', () => {
  assert.equal(isShieldState(178, 60), true);
  assert.equal(isShieldState(182, 1), true);
  assert.equal(isShieldState(179, 0), false);
  assert.equal(isShieldState(14, 60), false);
});

test('port shield colors follow Melee port assignment', () => {
  assert.deepEqual([...portShieldColor(0)], [1, 0.22, 0.28]);
  assert.deepEqual([...portShieldColor(1)], [0.22, 0.48, 1]);
  assert.deepEqual([...portShieldColor(2)], [1, 0.86, 0.16]);
  assert.deepEqual([...portShieldColor(3)], [0.22, 0.86, 0.38]);
  assert.deepEqual(portShieldColor(4), portShieldColor(0));
});

test('shine and firefox classify spacie specials from figatree names', () => {
  assert.equal(isSpacie(CHAR_FOX), true);
  assert.equal(isSpacie(CHAR_FALCO), true);
  assert.equal(isSpacie(0), false);
  assert.equal(isShineAction('PlyFox5K_Share_ACTION_SpecialLwStart_figatree'), true);
  assert.equal(isShineAction('PlyFalco5K_Share_ACTION_SpecialAirLwLoop_figatree'), true);
  assert.equal(isShineAction('PlyFox5K_Share_ACTION_Wait1_figatree'), false);
  assert.equal(isShineAction('PlyFox5K_Share_ACTION_FallSpecial_figatree'), false);
  assert.equal(isFirefoxAction('PlyFox5K_Share_ACTION_SpecialHi_figatree'), true);
  assert.equal(isFirefoxAction('PlyFalco5K_Share_ACTION_SpecialHiHoldAir_figatree'), true);
  assert.equal(isFirefoxAction('PlyFox5K_Share_ACTION_SpecialLwStart_figatree'), false);
});

test('Fox and Falco projectiles map to SLP item type ids', () => {
  assert.equal(isFoxLaser(0x36), true);
  assert.equal(isFalcoLaser(0x37), true);
  assert.equal(isFoxIllusion(0x38), true);
  assert.equal(isFalcoPhantasm(0x39), true);
  assert.equal(isFoxLaser(0x22), false);
  assert.ok(laserLength(7, 0) >= 10);
});

test('generated effect meshes are indexed triangles', () => {
  const sphere = createUvSphere(4, 6);
  assert.equal(sphere.positions.length % 3, 0);
  assert.equal(sphere.uvs.length, (sphere.positions.length / 3) * 2);
  assert.ok(sphere.indices.length >= 6);
  const hex = createHexPrism();
  assert.ok(hex.indices.length >= 24);
  const cylinder = createUnitCylinder(8);
  assert.ok(cylinder.indices.length >= 24);
});

test('effect catalog maps gfx aliases and item kinds onto extracted files', () => {
  const catalog = parseEffectCatalog({
    schema: 4,
    aliases: {
      shield: 'ef-co-11.model',
      shine: 'ef-fx-0.model',
      'fox-laser': 'it-54.model',
    },
    items: { 54: 'it-54.model', 55: 'it-55.model' },
    gfx: { 11: 'ef-co-11.model', 3000: 'ef-fx-0.model' },
  });
  assert.equal(catalog.aliases.shield, 'ef-co-11.model');
  assert.equal(catalog.aliases.shine, 'ef-fx-0.model');
  assert.equal(catalog.items['54'], 'it-54.model');
  assert.equal(catalog.gfx['3000'], 'ef-fx-0.model');
  assert.equal(effectAssetUrl(catalog.aliases.shield), '/assets/v4/models/ef-co-11.model?v=shield-mirror');
  assert.equal(parseEffectCatalog({
    schema: 4, aliases: { bad: '../secret.model' }, items: {}, gfx: {},
  }).aliases.bad, undefined);
  assert.throws(() => parseEffectCatalog({ schema: 3, aliases: {}, items: {}, gfx: {} }));
});

test('extracted DAT meshes scale to gameplay radii unless they are already close', () => {
  assert.equal(fitEffectScale(12, 12.5), 1);
  assert.ok(Math.abs(fitEffectScale(1, 12.5) - 12.5) < 1e-6);
  assert.equal(fitEffectScale(0, 10), 1);
  const positions = new Float32Array([3, 4, 0, -6, 0, 1]);
  assert.equal(xyRadius(positions), 6);
  assert.equal(xyExtent(positions), 6);
  assert.ok(Math.abs(xyExtent(new Float32Array([-0.394, 0.394, 0, 0.394, -0.394, 0])) - 0.394) < 1e-6);
});

test('shine DAT mesh is camera-facing XY, not fighter Z-forward', () => {
  const bytes = fs.readFileSync(cacheFile('ef-fx-0.model'));
  const model = parseModel(bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.byteLength));
  const bind = transformBindPose(model);
  let minX = Infinity, minY = Infinity, minZ = Infinity;
  let maxX = -Infinity, maxY = -Infinity, maxZ = -Infinity;
  for (let i = 0; i < bind.length; i += 3) {
    minX = Math.min(minX, bind[i]); maxX = Math.max(maxX, bind[i]);
    minY = Math.min(minY, bind[i + 1]); maxY = Math.max(maxY, bind[i + 1]);
    minZ = Math.min(minZ, bind[i + 2]); maxZ = Math.max(maxZ, bind[i + 2]);
  }
  const dx = maxX - minX, dy = maxY - minY, dz = maxZ - minZ;
  assert.ok(dx > 5 && dy > 5, 'shine spans the XY plane');
  assert.ok(dz < 0.05, 'shine has no fighter-forward thickness; swapping X/Z stands it on edge');
});

test('extracted shield is the in-game DAT billboard with a circular bubble texture', () => {
  const bytes = fs.readFileSync(cacheFile('ef-co-11.model'));
  const model = parseModel(bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.byteLength));
  assert.equal(model.vertexCount, 4);
  assert.equal(model.indexCount, 6);
  assert.ok(model.textures.length >= 2, 'I4 circle quadrant plus IA8 bubble lighting');
  assert.equal(model.textures[0].width, 64);
  assert.equal(model.textures[0].format, 0);
  assert.equal(model.textures[1].width, 128);
  assert.equal(model.textures[1].format, 3);
  const mask = model.textures[0];
  const at = (x, y) => mask.rgba[(y * mask.width + x) * 4];
  assert.ok(at(0, mask.height - 1) < 16, 'I4 quadrant is dark in the opposite corner');
  assert.ok(at(mask.width - 1, 0) > 200, 'I4 quadrant is bright at the circle center');
});
