import type { AnimationsAsset } from '../assets/anims.js';
import type { ModelAsset } from '../assets/model.js';

export const MATRIX_FLOATS = 12;
const BONE_TEXTURE_FLOATS = 24;

export function multiplyAffine(
  a: Float32Array, ao: number, b: Float32Array, bo: number,
  out: Float32Array, oo: number,
): void {
  for (let column = 0; column < 4; column++) {
    for (let row = 0; row < 3; row++) {
      let value = 0;
      for (let k = 0; k < 3; k++) value += a[ao + k * 3 + row] * b[bo + column * 3 + k];
      out[oo + column * 3 + row] = value;
    }
  }
  out[oo + 9] += a[ao + 9];
  out[oo + 10] += a[ao + 10];
  out[oo + 11] += a[ao + 11];
}

export function applyAffine(
  matrix: Float32Array, offset: number, x: number, y: number, z: number,
  out: Float32Array,
): void {
  out[0] = matrix[offset] * x + matrix[offset + 3] * y + matrix[offset + 6] * z + matrix[offset + 9];
  out[1] = matrix[offset + 1] * x + matrix[offset + 4] * y + matrix[offset + 7] * z + matrix[offset + 10];
  out[2] = matrix[offset + 2] * x + matrix[offset + 5] * y + matrix[offset + 8] * z + matrix[offset + 11];
}

function decomposeBase(model: ModelAsset, bone: number, scale: Float32Array, rotation: Float32Array, translation: Float32Array): void {
  const o = bone * MATRIX_FLOATS;
  const so = bone * 3;
  const m = model.boneBase;
  const sx = Math.hypot(m[o], m[o + 1], m[o + 2]);
  const sy = Math.hypot(m[o + 3], m[o + 4], m[o + 5]);
  const sz = Math.hypot(m[o + 6], m[o + 7], m[o + 8]);
  scale[so] = sx; scale[so + 1] = sy; scale[so + 2] = sz;
  translation[so] = m[o + 9]; translation[so + 1] = m[o + 10]; translation[so + 2] = m[o + 11];
  const m00 = m[o] / (sx > 1e-9 ? sx : 1);
  const m10 = m[o + 1] / (sx > 1e-9 ? sx : 1);
  const m20 = m[o + 2] / (sx > 1e-9 ? sx : 1);
  const m11 = m[o + 4] / (sy > 1e-9 ? sy : 1);
  const m21 = m[o + 5] / (sy > 1e-9 ? sy : 1);
  const m12 = m[o + 7] / (sz > 1e-9 ? sz : 1);
  const m22 = m[o + 8] / (sz > 1e-9 ? sz : 1);
  const ry = Math.asin(Math.max(-1, Math.min(1, -m20)));
  const cy = Math.cos(ry);
  let rx = 0, rz = 0;
  if (Math.abs(cy) > 1e-5) {
    rx = Math.atan2(m21, m22);
    rz = Math.atan2(m10, m00);
  } else {
    rx = Math.atan2(-m12, m11);
  }
  rotation[so] = rx; rotation[so + 1] = ry; rotation[so + 2] = rz;
}

function matrixFromSrt(
  sx: number, sy: number, sz: number, rx: number, ry: number, rz: number,
  tx: number, ty: number, tz: number, out: Float32Array, offset: number,
): void {
  const ti = rz * 0.5, tj = ry * 0.5, th = rx * 0.5;
  const si = Math.sin(ti), ci = Math.cos(ti), sj = Math.sin(tj), cj = Math.cos(tj);
  const sh = Math.sin(th), ch = Math.cos(th);
  const cc = ci * ch, cs = ci * sh, sc = si * ch, ss = si * sh;
  let qx = cj * cs - sj * sc, qy = cj * ss + sj * cc;
  let qz = cj * sc - sj * cs, qw = cj * cc + sj * ss;
  if (qw < 0) { qx = -qx; qy = -qy; qz = -qz; qw = -qw; }
  const x2 = qx + qx, y2 = qy + qy, z2 = qz + qz;
  const xx = qx * x2, xy = qx * y2, xz = qx * z2;
  const yy = qy * y2, yz = qy * z2, zz = qz * z2;
  const wx = qw * x2, wy = qw * y2, wz = qw * z2;
  out[offset] = (1 - (yy + zz)) * sx;
  out[offset + 1] = (xy + wz) * sx;
  out[offset + 2] = (xz - wy) * sx;
  out[offset + 3] = (xy - wz) * sy;
  out[offset + 4] = (1 - (xx + zz)) * sy;
  out[offset + 5] = (yz + wx) * sy;
  out[offset + 6] = (xz + wy) * sz;
  out[offset + 7] = (yz - wx) * sz;
  out[offset + 8] = (1 - (xx + yy)) * sz;
  out[offset + 9] = tx; out[offset + 10] = ty; out[offset + 11] = tz;
}

export interface AnimationTrack {
  startFrame: number;
  frames: Float32Array;
  values: Float32Array;
  inTangents: Float32Array;
  outTangents: Float32Array;
  interpolation: Uint8Array;
}

/** Match render.c sample_key, including its guarded Hermite fallback. */
export function sampleTrack(track: AnimationTrack, frame: number): number {
  const count = track.frames.length;
  if (!count) return 0;
  if (frame <= track.frames[0]) return track.values[0];
  if (frame >= track.frames[count - 1]) return track.values[count - 1];
  let low = 0, high = count - 1;
  while (low + 1 < high) {
    const middle = (low + high) >>> 1;
    if (track.frames[middle] <= frame) low = middle;
    else high = middle;
  }
  const span = track.frames[high] - track.frames[low];
  const t = span > 0 ? (frame - track.frames[low]) / span : 0;
  if (track.interpolation[low] === 1) return track.values[low];
  const linear = track.values[low] + (track.values[high] - track.values[low]) * t;
  if (track.interpolation[low] === 2) return linear;
  const t2 = t * t, t3 = t2 * t;
  const value = (2 * t3 - 3 * t2 + 1) * track.values[low]
    + (t3 - 2 * t2 + t) * track.outTangents[low] * span
    + (-2 * t3 + 3 * t2) * track.values[high]
    + (t3 - t2) * track.inTangents[high] * span;
  const lower = Math.min(track.values[low], track.values[high]);
  const upper = Math.max(track.values[low], track.values[high]);
  const margin = (upper - lower) * 2 + 1;
  return value < lower - margin || value > upper + margin ? linear : value;
}

export function findAction(animations: AnimationsAsset, name: string): number | null {
  if (!name) return null;
  const lower = name.toLowerCase();
  let match = animations.actions.findIndex(action => action.name.toLowerCase() === lower);
  if (match >= 0) return match;
  match = animations.actions.findIndex(action => action.name.startsWith(name));
  if (match >= 0) return match;
  match = animations.actions.findIndex(action => action.name.includes(name));
  return match >= 0 ? match : null;
}

export function resolveAction(animations: AnimationsAsset, requested: number): { index: number | null; fallback: boolean } {
  const index = resolveActionIndex(animations, requested);
  return { index, fallback: index !== requested };
}

function resolveActionIndex(animations: AnimationsAsset, requested: number): number | null {
  const action = requested >= 0 && requested < animations.actions.length ? animations.actions[requested] : null;
  return action?.joints.length ? requested : findAction(animations, 'Wait1');
}

function packRows(matrix: Float32Array, offset: number, output: Float32Array, outputOffset: number): void {
  output[outputOffset] = matrix[offset];
  output[outputOffset + 1] = matrix[offset + 3];
  output[outputOffset + 2] = matrix[offset + 6];
  output[outputOffset + 3] = matrix[offset + 9];
  output[outputOffset + 4] = matrix[offset + 1];
  output[outputOffset + 5] = matrix[offset + 4];
  output[outputOffset + 6] = matrix[offset + 7];
  output[outputOffset + 7] = matrix[offset + 10];
  output[outputOffset + 8] = matrix[offset + 2];
  output[outputOffset + 9] = matrix[offset + 5];
  output[outputOffset + 10] = matrix[offset + 8];
  output[outputOffset + 11] = matrix[offset + 11];
}

export interface PoseEvaluation {
  boneRows: Float32Array;
  requestedAction: number;
  resolvedAction: number | null;
  actionName: string | null;
  fallback: boolean;
}

/** Allocation-stable CPU bone evaluator; vertices remain entirely on the GPU. */
export class PoseEvaluator {
  readonly result: PoseEvaluation;
  private readonly local: Float32Array;
  private readonly world: Float32Array;
  private readonly skin: Float32Array;
  private readonly baseScale: Float32Array;
  private readonly baseRotation: Float32Array;
  private readonly baseTranslation: Float32Array;
  private readonly worldState: Uint8Array;

  constructor(readonly model: ModelAsset, readonly animations: AnimationsAsset) {
    this.local = new Float32Array(model.boneCount * MATRIX_FLOATS);
    this.world = new Float32Array(model.boneCount * MATRIX_FLOATS);
    this.skin = new Float32Array(MATRIX_FLOATS);
    this.baseScale = new Float32Array(model.boneCount * 3);
    this.baseRotation = new Float32Array(model.boneCount * 3);
    this.baseTranslation = new Float32Array(model.boneCount * 3);
    this.worldState = new Uint8Array(model.boneCount);
    for (let bone = 0; bone < model.boneCount; bone++) {
      const parent = model.boneParents[bone];
      if (parent !== 0xffff && parent >= model.boneCount) throw new Error(`model: bone ${bone} has invalid parent ${parent}`);
      decomposeBase(model, bone, this.baseScale, this.baseRotation, this.baseTranslation);
    }
    this.result = {
      boneRows: new Float32Array(model.boneCount * BONE_TEXTURE_FLOATS),
      requestedAction: 0xffffffff, resolvedAction: null, actionName: null, fallback: false,
    };
  }

  evaluate(requestedAction: number, animationFrame: number): PoseEvaluation {
    const resolvedIndex = resolveActionIndex(this.animations, requestedAction);
    return this.evaluatePose(requestedAction, resolvedIndex, resolvedIndex !== requestedAction, animationFrame);
  }

  evaluateBindPose(): PoseEvaluation {
    return this.evaluatePose(0xffffffff, null, false, 0);
  }

  private evaluatePose(requestedAction: number, resolvedIndex: number | null, fallback: boolean, animationFrame: number): PoseEvaluation {
    this.local.set(this.model.boneBase);
    const action = resolvedIndex === null ? null : this.animations.actions[resolvedIndex];
    if (action) {
      let frame = animationFrame <= 0 ? 1 : animationFrame + 1;
      if (action.loop && action.endFrame > 0) while (frame > action.endFrame) frame -= action.endFrame;
      const ledgeAnchored = action.name.includes('ACTION_Cliff');
      for (let jointIndex = 0; jointIndex < action.joints.length; jointIndex++) {
        const joint = action.joints[jointIndex];
        const bone = joint.boneIndex;
        if (bone >= this.model.boneCount) continue;
        const base = bone * 3;
        let sx = this.baseScale[base], sy = this.baseScale[base + 1], sz = this.baseScale[base + 2];
        let rx = this.baseRotation[base], ry = this.baseRotation[base + 1], rz = this.baseRotation[base + 2];
        let tx = this.baseTranslation[base], ty = this.baseTranslation[base + 1], tz = this.baseTranslation[base + 2];
        for (let trackIndex = 0; trackIndex < joint.tracks.length; trackIndex++) {
          const track = joint.tracks[trackIndex];
          if (frame < track.startFrame) continue;
          const value = sampleTrack(track, frame - track.startFrame);
          switch (track.channel) {
            case 1: rx = value; break; case 2: ry = value; break; case 3: rz = value; break;
            case 5: case 11: tx = value; break; case 6: ty = value; break; case 7: tz = value; break;
            case 8: sx = value; break; case 9: sy = value; break; case 10: sz = value; break;
          }
        }
        if (bone === 0) {
          tx = this.baseTranslation[0]; ty = this.baseTranslation[1]; tz = this.baseTranslation[2];
        }
        if (bone === 1 && !ledgeAnchored) tz = this.baseTranslation[5];
        matrixFromSrt(sx, sy, sz, rx, ry, rz, tx, ty, tz, this.local, bone * MATRIX_FLOATS);
      }
    }

    this.worldState.fill(0);
    for (let bone = 0; bone < this.model.boneCount; bone++) this.evaluateWorldBone(bone);
    for (let bone = 0; bone < this.model.boneCount; bone++) {
      const matrixOffset = bone * MATRIX_FLOATS;
      const textureOffset = bone * BONE_TEXTURE_FLOATS;
      packRows(this.world, matrixOffset, this.result.boneRows, textureOffset);
      multiplyAffine(this.world, matrixOffset, this.model.boneInverseWorld, matrixOffset, this.skin, 0);
      packRows(this.skin, 0, this.result.boneRows, textureOffset + MATRIX_FLOATS);
    }
    this.result.requestedAction = requestedAction;
    this.result.resolvedAction = resolvedIndex;
    this.result.actionName = action?.name ?? null;
    this.result.fallback = fallback;
    return this.result;
  }

  private evaluateWorldBone(bone: number): void {
    if (this.worldState[bone] === 2) return;
    if (this.worldState[bone] === 1) throw new Error(`model: bone cycle at ${bone}`);
    this.worldState[bone] = 1;
    const parent = this.model.boneParents[bone];
    const offset = bone * MATRIX_FLOATS;
    if (parent === 0xffff) {
      for (let index = 0; index < MATRIX_FLOATS; index++) this.world[offset + index] = this.local[offset + index];
    } else {
      this.evaluateWorldBone(parent);
      multiplyAffine(this.world, parent * MATRIX_FLOATS, this.local, offset, this.world, offset);
    }
    this.worldState[bone] = 2;
  }
}

function applyPackedRows(rows: Float32Array, offset: number, x: number, y: number, z: number, out: Float32Array): void {
  out[0] = rows[offset] * x + rows[offset + 1] * y + rows[offset + 2] * z + rows[offset + 3];
  out[1] = rows[offset + 4] * x + rows[offset + 5] * y + rows[offset + 6] * z + rows[offset + 7];
  out[2] = rows[offset + 8] * x + rows[offset + 9] * y + rows[offset + 10] * z + rows[offset + 11];
}

/** CPU oracle for tests only; production vertices are transformed by the shader. */
export function skinPositions(model: ModelAsset, boneRows: Float32Array, output = new Float32Array(model.positions.length)): Float32Array {
  const point = new Float32Array(3);
  for (let vertex = 0; vertex < model.vertexCount; vertex++) {
    const po = vertex * 3, wo = vertex * 4;
    const x = model.positions[po], y = model.positions[po + 1], z = model.positions[po + 2];
    let influences = 0;
    for (let k = 0; k < 4; k++) if (model.weights[wo + k] > 0.0001 && model.boneIndices[wo + k] < model.boneCount) influences++;
    let ax = 0, ay = 0, az = 0, totalWeight = 0;
    for (let k = 0; k < 4; k++) {
      const weight = model.weights[wo + k], bone = model.boneIndices[wo + k];
      if (weight <= 0.0001 || bone >= model.boneCount) continue;
      applyPackedRows(boneRows, bone * BONE_TEXTURE_FLOATS + (influences > 1 ? MATRIX_FLOATS : 0), x, y, z, point);
      ax += point[0] * weight; ay += point[1] * weight; az += point[2] * weight; totalWeight += weight;
    }
    output[po] = totalWeight > 0 ? ax / totalWeight : x;
    output[po + 1] = totalWeight > 0 ? ay / totalWeight : y;
    output[po + 2] = totalWeight > 0 ? az / totalWeight : z;
  }
  return output;
}
