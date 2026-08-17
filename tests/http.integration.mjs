import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';

import { parseTimeline } from '../web/dist/replay/timeline.js';

const baseUrl = process.env.DEV_URL;
if (!baseUrl) throw new Error('DEV_URL is required');

async function upload(bytes, name) {
  const response = await fetch(`${baseUrl}/api/replays`, {
    method: 'POST', body: bytes, headers: { 'X-Replay-Name': name },
  });
  if (response.status !== 201) throw new Error(`upload failed ${response.status}: ${await response.text()}`);
  return (await response.json()).id;
}

const firstBytes = await readFile('fixtures/vertical.slp');
const iceClimbersBytes = await readFile('fixtures/ICs.slp');

const [firstId, secondId] = await Promise.all([
  upload(firstBytes, 'vertical.slp'), upload(iceClimbersBytes, 'ICs.slp'),
]);
assert.notEqual(firstId, secondId);

async function load(id) {
  const [manifestResponse, timelineResponse] = await Promise.all([
    fetch(`${baseUrl}/api/replays/${id}/manifest`),
    fetch(`${baseUrl}/api/replays/${id}/timeline`),
  ]);
  assert.equal(manifestResponse.status, 200);
  assert.equal(timelineResponse.status, 200);
  const manifest = await manifestResponse.json();
  const buffer = await timelineResponse.arrayBuffer();
  return { manifest, timeline: parseTimeline(buffer), bytes: buffer.byteLength,
    etag: timelineResponse.headers.get('etag') };
}

const [first, second] = await Promise.all([load(firstId), load(secondId)]);
assert.equal(first.manifest.id, firstId);
assert.equal(second.manifest.id, secondId);
assert.equal(first.manifest.name, 'vertical.slp');
assert.equal(second.manifest.name, 'ICs.slp');
assert.equal(first.timeline.players[0].name, 'liteyear');
assert.equal(second.timeline.players[1].name, 'Two Krillins');
assert.ok(first.bytes < 2_000_000, `timeline is ${first.bytes} bytes`);
assert.notEqual(first.timeline.frameCount, second.timeline.frameCount);

const cached = await fetch(`${baseUrl}/api/replays/${firstId}/timeline`, {
  headers: { 'If-None-Match': first.etag },
});
assert.equal(cached.status, 304);

const assetUrl = first.manifest.assets.find(asset => asset.model)?.model;
const asset = await fetch(`${baseUrl}${assetUrl}`);
assert.equal(asset.status, 200);
assert.equal(asset.headers.get('content-type'), 'application/vnd.melee.model');
assert.match(asset.headers.get('cache-control'), /immutable/);

const moduleResponse = await fetch(`${baseUrl}/main.js`);
assert.equal(moduleResponse.status, 200);
assert.match(moduleResponse.headers.get('content-type'), /text\/javascript/);
assert.match(await moduleResponse.text(), /parseTimeline/);
const animationModule = await fetch(`${baseUrl}/animation/pose.js`);
assert.equal(animationModule.status, 200);
assert.match(animationModule.headers.get('content-type'), /text\/javascript/);
const sceneModule = await fetch(`${baseUrl}/replay/scene.js`);
assert.equal(sceneModule.status, 200);
assert.match(sceneModule.headers.get('content-type'), /text\/javascript/);

const [softwarePage, webglPage] = await Promise.all([
  fetch(`${baseUrl}/`).then(response => response.text()),
  fetch(`${baseUrl}/?renderer=webgl2`).then(response => response.text()),
]);
assert.match(softwarePage, /Melee 2D Replay/);
assert.doesNotMatch(softwarePage, /data-renderer="webgl2"/);
assert.match(webglPage, /data-renderer="webgl2"/);
assert.match(webglPage, /Melee WebGL2 replay renderer/);

const iceClimbersId = secondId;
const iceManifest = second.manifest;
const nanaAsset = iceManifest.assets.find(asset => asset.slot === 3);
assert.equal(nanaAsset.model, '/assets/v4/models/nana-3.model');
assert.equal(nanaAsset.animations, '/assets/v4/anims/popo-3.anims');
const iceTimeline = second.timeline;
assert.equal(iceTimeline.slots[3].active, true);
assert.equal(iceTimeline.slots[3].presence.reduce((sum, value) => sum + value, 0), 6179);
assert.equal(iceTimeline.frame(3, 173).character, 11);
assert.equal(iceTimeline.frame(3, 173).animationIndex, 2);
assert.ok(second.bytes < 2_000_000);

const replayList = await (await fetch(`${baseUrl}/api/replays`)).json();
assert.equal(replayList.find(replay => replay.id === firstId).name, 'vertical.slp');
assert.equal(replayList.find(replay => replay.id === iceClimbersId).name, 'ICs.slp');

console.log(`HTTP integration passed: renderer flag isolation, distinct replays, ${first.bytes} byte timeline, Nana shared action bank`);
