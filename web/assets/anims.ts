import { BinaryReader, boundedCount, decodeCString } from './binary.js';
import { ASSET_MAGIC, ASSET_SCHEMA } from './model.js';

const LIMITS = Object.freeze({ actions: 4096, joints: 4096, tracks: 64, keys: 2_000_000 });

export interface JointSpline {
  type: number;
  ncv: number;
  tension: number;
  points: Float32Array;
}

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
    const joints: {
      boneIndex: number;
      tracks: {
        channel: number; startFrame: number; frames: Float32Array; values: Float32Array;
        inTangents: Float32Array; outTangents: Float32Array; interpolation: Uint8Array;
      }[];
      spline: JointSpline | null;
    }[] = [];
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
      joints.push({ boneIndex, tracks, spline: null });
    }
    actions.push({ name, endFrame, loop, joints });
  }
  const splines = parseSplineTrailer(reader);
  for (const spline of splines) {
    const action = actions[spline.action];
    const joint = action?.joints[spline.joint];
    if (!joint) throw new RangeError(`animations: spline ${spline.action}/${spline.joint} is out of range`);
    joint.spline = { type: spline.type, ncv: spline.ncv, tension: spline.tension, points: spline.points };
  }
  if (reader.offset !== buffer.byteLength) throw new Error(`animations: ${buffer.byteLength - reader.offset} trailing bytes`);
  return { schema, actions, keyCount: totalKeys };
}

const SPLINE_MAGIC = 0x53504c4e;

function parseSplineTrailer(reader: BinaryReader) {
  if (reader.offset === reader.view.byteLength) return [];
  if (reader.offset + 8 > reader.view.byteLength) throw new Error('animations: truncated spline trailer');
  const magic = reader.u32();
  if (magic !== SPLINE_MAGIC) {
    throw new Error(`animations: ${reader.view.byteLength - (reader.offset - 4)} trailing bytes`);
  }
  const count = boundedCount(reader.u32(), LIMITS.joints * LIMITS.actions, 'animation splines');
  const splines = [];
  for (let i = 0; i < count; i++) {
    const action = reader.u32();
    const joint = reader.u32();
    const type = reader.u8();
    reader.u8();
    const ncv = reader.u16();
    const nvec = boundedCount(reader.u32(), 1024, `spline ${i} points`);
    const tension = reader.f32();
    const points = new Float32Array(nvec * 3);
    for (let p = 0; p < points.length; p++) points[p] = reader.f32();
    splines.push({ action, joint, type, ncv, tension, points });
  }
  return splines;
}

export type AnimationsAsset = ReturnType<typeof parseAnimations>;
