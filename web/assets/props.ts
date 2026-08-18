import { BinaryReader, boundedCount, decodeCString } from './binary.js';
import { ASSET_MAGIC, ASSET_SCHEMA } from './model.js';
import { parseModelFromReader } from './model.js';

const LIMITS = Object.freeze({
  props: 64, actions: 256, joints: 1024, tracks: 64, keys: 2_000_000,
});

/** Parse an anims block from the current reader position (used by props). */
function parseAnimsFromReader(reader: BinaryReader) {
  const magic = reader.u32();
  if (magic !== ASSET_MAGIC) throw new Error('props: bad anims magic');
  const schema = reader.u32();
  if (schema !== ASSET_SCHEMA) throw new Error('props: unsupported anims schema');
  const actionCount = boundedCount(reader.u32(), LIMITS.actions, 'props animation actions');
  const actions = [];
  for (let i = 0; i < actionCount; i++) {
    const name = decodeCString(reader.bytes(48));
    const endFrame = reader.f32();
    const loop = reader.u8() !== 0;
    reader.u8(); reader.u16();
    const jointCount = boundedCount(reader.u32(), LIMITS.joints, `props action ${i} joints`);
    const joints = [];
    for (let j = 0; j < jointCount; j++) {
      const boneIndex = reader.u16();
      const trackCount = boundedCount(reader.u32(), LIMITS.tracks, `props action ${i} joint ${j} tracks`);
      const tracks = [];
      for (let t = 0; t < trackCount; t++) {
        const channel = reader.u8();
        const startFrame = reader.u16();
        const keyCount = boundedCount(reader.u32(), LIMITS.keys, `props action ${i} keys`);
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
  return { actions };
}

/** Parse the per-stage `.props` binary produced by the extractor. */
export function parseProps(buffer: ArrayBuffer) {
  const reader = new BinaryReader(buffer);
  const magic = reader.u32();
  if (magic !== ASSET_MAGIC) throw new Error('props: bad magic');
  const schema = reader.u32();
  if (schema !== ASSET_SCHEMA) throw new Error(`props: unsupported asset schema ${schema}`);
  const propCount = boundedCount(reader.u32(), LIMITS.props, 'props count');
  const props = [];
  for (let i = 0; i < propCount; i++) {
    const kind = reader.u32();
    const position = new Float32Array([reader.f32(), reader.f32(), reader.f32()]);
    const model = parseModelFromReader(reader, `prop ${i} model`);
    const { actions } = parseAnimsFromReader(reader);
    props.push({ kind, position, model, actions });
  }
  if (reader.offset !== buffer.byteLength) throw new Error(`props: ${buffer.byteLength - reader.offset} trailing bytes`);
  return { schema, props };
}

export type PropsAsset = ReturnType<typeof parseProps>;
