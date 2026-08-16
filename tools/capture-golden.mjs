import { readFile, writeFile, mkdir } from 'node:fs/promises';
import { basename } from 'node:path';

import { parseTimeline } from '../web/dist/replay/timeline.js';
import { parseAnimations } from '../web/dist/assets/anims.js';

const baseUrl = process.env.DEV_URL;
if (!baseUrl) throw new Error('DEV_URL is required (source dev-server.env)');
const replayPath = process.argv[2] || 'fixtures/vertical.slp';
const outputDir = process.argv[3] || 'build/golden';
const iceClimbers = /(^|\/)ics\.slp$/i.test(replayPath);
const fixtures = iceClimbers ? [
  ['nana-shield', 42], ['nana-airborne', 61], ['nana-idle', 173],
  ['nana-attack', 295], ['nana-facing-flip', 444], ['nana-separated', 2302],
] : [
  ['idle', 8], ['dash', 11], ['airborne', 18], ['shield', 99],
  ['facing-flip', 275], ['run', 1004], ['attack', 1852],
];

await mkdir(outputDir, { recursive: true });
const replayBytes = await readFile(replayPath);
const created = await fetch(`${baseUrl}/api/replays`, {
  method: 'POST', body: replayBytes,
  headers: { 'X-Replay-Name': basename(replayPath) },
});
if (!created.ok) throw new Error(`upload failed: ${created.status} ${await created.text()}`);
const { id } = await created.json();
const manifestResponse = await fetch(`${baseUrl}/api/replays/${id}/manifest`);
if (!manifestResponse.ok) throw new Error(`manifest failed: ${manifestResponse.status}`);
const manifest = await manifestResponse.json();
const timelineResponse = await fetch(`${baseUrl}/api/replays/${id}/timeline`);
if (!timelineResponse.ok) throw new Error(`timeline failed: ${timelineResponse.status}`);
const timeline = parseTimeline(await timelineResponse.arrayBuffer());

const animationDiagnostics = [];
for (let slotIndex = 0; slotIndex < timeline.slots.length; slotIndex++) {
  const slot = timeline.slots[slotIndex];
  if (!slot.active) continue;
  const asset = manifest.assets.find(entry => entry.slot === slotIndex);
  if (!asset?.animations) throw new Error(`slot ${slotIndex} has no animation asset`);
  const response = await fetch(`${baseUrl}${asset.animations}`);
  if (!response.ok) throw new Error(`animations for slot ${slotIndex} failed: ${response.status}`);
  const animations = parseAnimations(await response.arrayBuffer());
  let present = 0, missing = 0;
  for (let i = 0; i < timeline.frameCount; i++) {
    if (!slot.presence[i]) continue;
    present++;
    const index = slot.animationIndex[i];
    if (index === 0xffffffff) { missing++; continue; }
    if (index >= animations.actions.length) {
      throw new Error(`slot ${slotIndex} frame ${timeline.startFrame + i}: animation ${index} >= ${animations.actions.length}`);
    }
  }
  animationDiagnostics.push({ slot: slotIndex, presentFrames: present,
    resolvedFrames: present - missing, missingFrames: missing,
    selected: fixtures.map(([label, frame]) => {
      const index = frame - timeline.startFrame;
      const animationIndex = slot.animationIndex[index];
      return { label, frame, actionState: slot.actionState[index], animationIndex,
        action: animationIndex === 0xffffffff ? null : animations.actions[animationIndex].name };
    }) });
}
await writeFile(`${outputDir}/animation-index.json`, `${JSON.stringify(animationDiagnostics, null, 2)}\n`);

for (const [label, frame] of fixtures) {
  const index = frame - timeline.startFrame;
  const state = {
    replayId: id, frame, stageId: timeline.stageId,
    slots: timeline.slots.map((_, slot) => timeline.frame(slot, frame)).filter(Boolean),
    camera: timeline.camera ? {
      x: timeline.camera.x[index], y: timeline.camera.y[index], zoom: timeline.camera.zoom[index],
    } : null,
  };
  await writeFile(`${outputDir}/${label}-${frame}.json`, `${JSON.stringify(state, null, 2)}\n`);
  const image = await fetch(`${baseUrl}/api/replays/${id}/reference?n=${frame}`);
  if (!image.ok) throw new Error(`reference ${frame} failed: ${image.status} ${await image.text()}`);
  await writeFile(`${outputDir}/${label}-${frame}.png`, new Uint8Array(await image.arrayBuffer()));
}
console.log(`captured ${fixtures.length} C state/image pairs in ${outputDir} for ${id}`);
