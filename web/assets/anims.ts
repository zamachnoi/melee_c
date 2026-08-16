import { BinaryReader, boundedCount, decodeCString } from './binary.js';
import { ASSET_MAGIC, ASSET_SCHEMA } from './model.js';

const LIMITS = Object.freeze({ actions: 4096, joints: 4096, tracks: 64, keys: 2_000_000 });

export function parseAnimations(buffer: ArrayBuffer) {
  const reader = new BinaryReader(buffer);
  const magic = reader.u32();
  if (magic !== ASSET_MAGIC) throw new Error('animations: bad magic');
  const schema = reader.u32();
  if (schema !== ASSET_SCHEMA) throw new Error(`animations: unsupported asset schema ${schema}`);
  const actionCount = boundedCount(reader.u32(), LIMITS.actions, 'animation actions');
  let totalKeys = 0;
  const actions = [];
  for (let i = 0; i < actionCount; i++) {
    const name = decodeCString(reader.bytes(48));
    const endFrame = reader.f32();
    const loop = reader.u8() !== 0;
    reader.u8(); reader.u16();
    const jointCount = boundedCount(reader.u32(), LIMITS.joints, `action ${i} joints`);
    const joints = [];
    for (let j = 0; j < jointCount; j++) {
      const boneIndex = reader.u16();
      const trackCount = boundedCount(reader.u32(), LIMITS.tracks, `action ${i} joint ${j} tracks`);
      const tracks = [];
      for (let t = 0; t < trackCount; t++) {
        const channel = reader.u8();
        const startFrame = reader.u16();
        const keyCount = boundedCount(reader.u32(), LIMITS.keys - totalKeys, `action ${i} keys`);
        totalKeys += keyCount;
        const frames = new Float32Array(keyCount);
        const values = new Float32Array(keyCount);
        const inTangents = new Float32Array(keyCount);
        const outTangents = new Float32Array(keyCount);
        const interpolation = new Uint8Array(keyCount);
        for (let k = 0; k < keyCount; k++) {
          frames[k] = reader.f32(); values[k] = reader.f32();
          inTangents[k] = reader.f32(); outTangents[k] = reader.f32();
          interpolation[k] = reader.u8();
        }
        tracks.push({ channel, startFrame, frames, values, inTangents, outTangents, interpolation });
      }
      joints.push({ boneIndex, tracks });
    }
    actions.push({ name, endFrame, loop, joints });
  }
  if (reader.offset !== buffer.byteLength) throw new Error(`animations: ${buffer.byteLength - reader.offset} trailing bytes`);
  return { schema, actions, keyCount: totalKeys };
}
