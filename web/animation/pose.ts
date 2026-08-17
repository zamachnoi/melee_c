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

/** Fighter DAT (Z forward, Y up, X side) → gameplay (X along the stage, Y up, Z depth). */
const FIGHTER_TO_GAMEPLAY = new Float32Array([
  0, 0, 1, 0, 1, 0, 1, 0, 0, 0, 0, 0,
]);

export function fighterPositionsToGameplay(
  positions: Float32Array, output: Float32Array = new Float32Array(positions.length),
): Float32Array {
  for (let i = 0; i < positions.length; i += 3) {
    const x = positions[i], y = positions[i + 1], z = positions[i + 2];
    output[i] = z; output[i + 1] = y; output[i + 2] = x;
  }
  return output;
}

function leftMultiplyAffine(
  left: Float32Array, world: Float32Array, offset: number, temp: Float32Array,
): void {
  multiplyAffine(left, 0, world, offset, temp, 0);
  world.set(temp.subarray(0, MATRIX_FLOATS), offset);
}

function scaleAffineInPlace(world: Float32Array, offset: number, scale: number): void {
  if (scale === 1) return;
  for (let i = 0; i < MATRIX_FLOATS; i++) world[offset + i] *= scale;
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

function splinePoint(cv: Float32Array, index: number, out: Float32Array): void {
  const o = index * 3;
  out[0] = cv[o] ?? 0; out[1] = cv[o + 1] ?? 0; out[2] = cv[o + 2] ?? 0;
}

function blendSpline(cv: Float32Array, cp: number, b0: number, b1: number, b2: number, b3: number, out: Float32Array): void {
  out[0] = cv[cp] * b0 + cv[cp + 3] * b1 + cv[cp + 6] * b2 + cv[cp + 9] * b3;
  out[1] = cv[cp + 1] * b0 + cv[cp + 4] * b1 + cv[cp + 7] * b2 + cv[cp + 10] * b3;
  out[2] = cv[cp + 2] * b0 + cv[cp + 5] * b1 + cv[cp + 8] * b2 + cv[cp + 11] * b3;
}

function sampleBSpline(cv: Float32Array, idx: number, t: number, out: Float32Array): void {
  const t2 = t * t, t3 = t2 * t, u1 = 1 - t, k = 1 / 6;
  blendSpline(cv, idx * 3, k * u1 * u1 * u1, k * (4 + (3 * t3 - 6 * t2)), k * (3 * (-t3 + t2 + t) + 1), k * t3, out);
}

/** `splGetSplinePoint`: parameter `u` in [0, 1] along HSD_Spline CVs. */
export function evaluateSpline(
  type: number, ncv: number, tension: number, cv: Float32Array, u: number, out: Float32Array,
): void {
  if (ncv < 1 || cv.length < 3) { out[0] = 0; out[1] = 0; out[2] = 0; return; }
  if (u <= 0) { splinePoint(cv, 0, out); return; }
  if (u >= 1) {
    if (type === 1) splinePoint(cv, (ncv - 1) * 3, out);
    else if (type === 2) sampleBSpline(cv, Math.max(0, ncv - 2), 1, out);
    else if (type === 3) splinePoint(cv, ncv, out);
    else splinePoint(cv, ncv - 1, out);
    return;
  }
  const scaled = u * (ncv - 1);
  const idx = Math.min(ncv - 2, Math.max(0, Math.floor(scaled)));
  const t = scaled - idx;
  if (type === 0) {
    const a = idx * 3, b = (idx + 1) * 3;
    out[0] = cv[a] + t * (cv[b] - cv[a]);
    out[1] = cv[a + 1] + t * (cv[b + 1] - cv[a + 1]);
    out[2] = cv[a + 2] + t * (cv[b + 2] - cv[a + 2]);
    return;
  }
  if (type === 1) {
    const u1 = 1 - t, t2 = t * t, u12 = u1 * u1;
    blendSpline(cv, idx * 9, u12 * u1, 3 * t * u12, 3 * t2 * u1, t2 * t, out);
    return;
  }
  const cp = idx * 3;
  if (type === 2) {
    sampleBSpline(cv, idx, t, out);
    return;
  }
  const t2 = t * t, t3 = t2 * t;
  blendSpline(
    cv, cp,
    tension * (-t3 + 2 * t2 - t),
    ((2 - tension) * t3) + ((tension - 3) * t2) + 1,
    ((tension - 2) * t3) + ((3 - 2 * tension) * t2) + tension * t,
    tension * (t3 - t2),
    out,
  );
}

const pathScratchA = new Float32Array(3);
const pathScratchB = new Float32Array(3);

/** Invert HSD PATH `s` (fraction of arc length) to a spline parameter, then sample. */
export function evaluatePathSpline(
  type: number, ncv: number, tension: number, cv: Float32Array, s: number, out: Float32Array,
): void {
  const path = s < 0 ? 0 : s > 1 ? 1 : s;
  if (ncv < 2) { evaluateSpline(type, ncv, tension, cv, path, out); return; }
  if (type === 0) {
    let total = 0;
    const spans = new Float32Array(ncv);
    for (let i = 1; i < ncv; i++) {
      const a = (i - 1) * 3, b = i * 3;
      const dx = cv[b] - cv[a], dy = cv[b + 1] - cv[a + 1], dz = cv[b + 2] - cv[a + 2];
      total += Math.hypot(dx, dy, dz);
      spans[i] = total;
    }
    if (!(total > 0)) { evaluateSpline(type, ncv, tension, cv, path, out); return; }
    const target = path * total;
    let idx = 1;
    while (idx < ncv - 1 && spans[idx] < target) idx++;
    const prev = spans[idx - 1];
    const span = spans[idx] - prev;
    const t = span > 1e-12 ? (target - prev) / span : 0;
    evaluateSpline(type, ncv, tension, cv, (idx - 1 + t) / (ncv - 1), out);
    return;
  }
  const steps = Math.max(8, (ncv - 1) * 8);
  let total = 0;
  evaluateSpline(type, ncv, tension, cv, 0, pathScratchA);
  const samplesU = new Float32Array(steps + 1);
  const samplesS = new Float32Array(steps + 1);
  samplesU[0] = 0; samplesS[0] = 0;
  for (let i = 1; i <= steps; i++) {
    const u = i / steps;
    evaluateSpline(type, ncv, tension, cv, u, pathScratchB);
    total += Math.hypot(
      pathScratchB[0] - pathScratchA[0],
      pathScratchB[1] - pathScratchA[1],
      pathScratchB[2] - pathScratchA[2],
    );
    samplesU[i] = u; samplesS[i] = total;
    pathScratchA[0] = pathScratchB[0]; pathScratchA[1] = pathScratchB[1]; pathScratchA[2] = pathScratchB[2];
  }
  if (!(total > 0)) { evaluateSpline(type, ncv, tension, cv, path, out); return; }
  const target = path * total;
  let idx = 1;
  while (idx < steps && samplesS[idx] < target) idx++;
  const prev = samplesS[idx - 1];
  const span = samplesS[idx] - prev;
  const t = span > 1e-12 ? (target - prev) / span : 0;
  evaluateSpline(type, ncv, tension, cv, samplesU[idx - 1] + t * (samplesU[idx] - samplesU[idx - 1]), out);
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
      for (let jointIndex = 0; jointIndex < action.joints.length; jointIndex++) {
        const joint = action.joints[jointIndex];
        const bone = joint.boneIndex;
        if (bone >= this.model.boneCount) continue;
        const base = bone * 3;
        let sx = this.baseScale[base], sy = this.baseScale[base + 1], sz = this.baseScale[base + 2];
        let rx = this.baseRotation[base], ry = this.baseRotation[base + 1], rz = this.baseRotation[base + 2];
        let tx = this.baseTranslation[base], ty = this.baseTranslation[base + 1], tz = this.baseTranslation[base + 2];
        let pathU = Number.NaN;
        for (let trackIndex = 0; trackIndex < joint.tracks.length; trackIndex++) {
          const track = joint.tracks[trackIndex];
          if (frame < track.startFrame) continue;
          const value = sampleTrack(track, frame - track.startFrame);
          switch (track.channel) {
            case 1: rx = value; break; case 2: ry = value; break; case 3: rz = value; break;
            case 4: pathU = value; break;
            case 5: case 11: tx = value; break; case 6: ty = value; break; case 7: tz = value; break;
            case 8: sx = value; break; case 9: sy = value; break; case 10: sz = value; break;
          }
        }
        const spline = joint.spline;
        if (spline && Number.isFinite(pathU)) {
          const point = this.skin;
          evaluatePathSpline(spline.type, spline.ncv, spline.tension, spline.points, pathU, point);
          tx = point[0]; ty = point[1]; tz = point[2];
        }
        if (bone === 0) {
          tx = this.baseTranslation[0]; ty = this.baseTranslation[1]; tz = this.baseTranslation[2];
        }
        /* TransN translation is already in the Slippi root; keep rotation/scale. */
        if (bone === 1) {
          tx = this.baseTranslation[3]; ty = this.baseTranslation[4]; tz = this.baseTranslation[5];
        }
        matrixFromSrt(sx, sy, sz, rx, ry, rz, tx, ty, tz, this.local, bone * MATRIX_FLOATS);
      }
    }

    this.worldState.fill(0);
    for (let bone = 0; bone < this.model.boneCount; bone++) this.evaluateWorldBone(bone);
    for (let bone = 0; bone < this.model.boneCount; bone++) {
      const matrixOffset = bone * MATRIX_FLOATS;
      const textureOffset = bone * BONE_TEXTURE_FLOATS;
      leftMultiplyAffine(FIGHTER_TO_GAMEPLAY, this.world, matrixOffset, this.skin);
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

/** Skin model-local positions with packed bone rows (world or inverse-bind). */
export function skinPositions(
  model: ModelAsset, boneRows: Float32Array,
  output: Float32Array<ArrayBufferLike> = new Float32Array(model.positions.length),
): Float32Array {
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

export const STAGE_BONE_FLOATS = BONE_TEXTURE_FLOATS;

interface StagePoseScratch {
  boneCount: number;
  local: Float32Array;
  world: Float32Array;
  skin: Float32Array;
  scale: Float32Array;
  rotation: Float32Array;
  translation: Float32Array;
  state: Uint8Array;
}

let stagePoseScratch: StagePoseScratch | null = null;

function acquireStagePoseScratch(boneCount: number): StagePoseScratch {
  const current = stagePoseScratch;
  if (current && current.boneCount >= boneCount) return current;
  stagePoseScratch = {
    boneCount,
    local: new Float32Array(boneCount * MATRIX_FLOATS),
    world: new Float32Array(boneCount * MATRIX_FLOATS),
    skin: new Float32Array(MATRIX_FLOATS),
    scale: new Float32Array(boneCount * 3),
    rotation: new Float32Array(boneCount * 3),
    translation: new Float32Array(boneCount * 3),
    state: new Uint8Array(boneCount),
  };
  return stagePoseScratch;
}

/** Convert a gameplay/world Y into a DAT-local bone translation delta.  Prefer
    `evaluateStagePose`'s gameplay mesh-top map; this remains for DAT-local edits. */
export function stageBoneYOffset(worldY: number, stageScale: number): number {
  return stageScale === 0 ? worldY : worldY / stageScale;
}

function wrapStageFrame(frame: number, action: AnimationsAsset['actions'][number]): number {
  const end = action.endFrame;
  if (action.loop && end > 0) {
    let t = frame % end;
    if (t < 0) t += end;
    return t;
  }
  return frame;
}

function boneIsUnder(model: ModelAsset, bone: number, ancestor: number): boolean {
  let current = bone;
  for (let step = 0; step < model.boneCount; step++) {
    if (current === ancestor) return true;
    if (current === 0xffff) return false;
    current = model.boneParents[current];
  }
  return false;
}

function meshTopY(model: ModelAsset, world: Float32Array, ancestor: number, point: Float32Array, skin: Float32Array): number {
  let maxY = -Infinity;
  for (let vertex = 0; vertex < model.vertexCount; vertex++) {
    const wo = vertex * 4;
    let follows = false;
    for (let k = 0; k < 4; k++) {
      const bone = model.boneIndices[wo + k];
      if (model.weights[wo + k] > 0.0001 && bone < model.boneCount && boneIsUnder(model, bone, ancestor)) {
        follows = true; break;
      }
    }
    if (!follows) continue;
    const po = vertex * 3;
    const x = model.positions[po], y = model.positions[po + 1], z = model.positions[po + 2];
    let influences = 0;
    for (let k = 0; k < 4; k++) {
      if (model.weights[wo + k] > 0.0001 && model.boneIndices[wo + k] < model.boneCount) influences++;
    }
    let ax = 0, ay = 0, az = 0, totalWeight = 0;
    for (let k = 0; k < 4; k++) {
      const weight = model.weights[wo + k], bone = model.boneIndices[wo + k];
      if (weight <= 0.0001 || bone >= model.boneCount) continue;
      if (influences > 1) {
        multiplyAffine(world, bone * MATRIX_FLOATS, model.boneInverseWorld, bone * MATRIX_FLOATS, skin, 0);
        applyAffine(skin, 0, x, y, z, point);
      } else {
        applyAffine(world, bone * MATRIX_FLOATS, x, y, z, point);
      }
      ax += point[0] * weight; ay += point[1] * weight; az += point[2] * weight; totalWeight += weight;
    }
    if (totalWeight > 0 && ay / totalWeight > maxY) maxY = ay / totalWeight;
  }
  return maxY;
}

function snapMeshTopsToWorldY(model: ModelAsset, world: Float32Array, boneTops: ReadonlyMap<number, number>): void {
  const point = new Float32Array(3);
  const skin = new Float32Array(MATRIX_FLOATS);
  for (const [bone, target] of boneTops) {
    if (bone < 0 || bone >= model.boneCount || !Number.isFinite(target)) continue;
    let top = meshTopY(model, world, bone, point, skin);
    if (!Number.isFinite(top)) top = world[bone * MATRIX_FLOATS + 10];
    const delta = target - top;
    if (delta === 0) continue;
    for (let other = 0; other < model.boneCount; other++) {
      if (boneIsUnder(model, other, bone)) world[other * MATRIX_FLOATS + 10] += delta;
    }
  }
}

function packScaledStagePose(
  model: ModelAsset, world: Float32Array, skin: Float32Array, rows: Float32Array,
  scale: number, boneTops?: ReadonlyMap<number, number>,
): void {
  for (let bone = 0; bone < model.boneCount; bone++) {
    scaleAffineInPlace(world, bone * MATRIX_FLOATS, scale);
  }
  if (boneTops && boneTops.size) snapMeshTopsToWorldY(model, world, boneTops);
  for (let bone = 0; bone < model.boneCount; bone++) {
    const matrixOffset = bone * MATRIX_FLOATS;
    const textureOffset = bone * BONE_TEXTURE_FLOATS;
    packRows(world, matrixOffset, rows, textureOffset);
    multiplyAffine(world, matrixOffset, model.boneInverseWorld, matrixOffset, skin, 0);
    packRows(skin, 0, rows, textureOffset + MATRIX_FLOATS);
  }
}

/** Sample a map_head AnimJoint clip onto a stage section in gameplay space.
    Unlike fighter PoseEvaluator this does not pin bones 0/1. */
export function evaluateStageAnim(
  model: ModelAsset,
  action: AnimationsAsset['actions'][number] | undefined,
  frame: number,
  output?: Float32Array,
  stageScale = 1,
): Float32Array {
  const rows = output ?? new Float32Array(model.boneCount * BONE_TEXTURE_FLOATS);
  const scratch = acquireStagePoseScratch(model.boneCount);
  const { local, world, skin, scale, rotation, translation, state } = scratch;
  local.set(model.boneBase);
  for (let bone = 0; bone < model.boneCount; bone++) decomposeBase(model, bone, scale, rotation, translation);
  if (action?.joints.length) {
    const t = wrapStageFrame(frame, action);
    for (let jointIndex = 0; jointIndex < action.joints.length; jointIndex++) {
      const joint = action.joints[jointIndex];
      const bone = joint.boneIndex;
      if (bone >= model.boneCount) continue;
      const base = bone * 3;
      let sx = scale[base], sy = scale[base + 1], sz = scale[base + 2];
      let rx = rotation[base], ry = rotation[base + 1], rz = rotation[base + 2];
      let tx = translation[base], ty = translation[base + 1], tz = translation[base + 2];
      let pathU = Number.NaN;
      for (let trackIndex = 0; trackIndex < joint.tracks.length; trackIndex++) {
        const track = joint.tracks[trackIndex];
        if (t < track.startFrame) continue;
        const value = sampleTrack(track, t - track.startFrame);
        switch (track.channel) {
          case 1: rx = value; break; case 2: ry = value; break; case 3: rz = value; break;
          case 4: pathU = value; break;
          case 5: case 11: tx = value; break; case 6: ty = value; break; case 7: tz = value; break;
          case 8: sx = value; break; case 9: sy = value; break; case 10: sz = value; break;
        }
      }
      const spline = joint.spline;
      if (spline && Number.isFinite(pathU)) {
        const path = pathU < 0 ? 0 : pathU > 1 ? 1 : pathU;
        const point = scratch.skin;
        evaluatePathSpline(spline.type, spline.ncv, spline.tension, spline.points, path, point);
        tx = point[0]; ty = point[1]; tz = point[2];
      }
      matrixFromSrt(sx, sy, sz, rx, ry, rz, tx, ty, tz, local, bone * MATRIX_FLOATS);
    }
  }
  state.fill(0);
  const evaluateWorldBone = (bone: number): void => {
    if (state[bone] === 2) return;
    if (state[bone] === 1) return;
    state[bone] = 1;
    const parent = model.boneParents[bone];
    const offset = bone * MATRIX_FLOATS;
    if (parent === 0xffff) {
      for (let index = 0; index < MATRIX_FLOATS; index++) world[offset + index] = local[offset + index];
    } else {
      evaluateWorldBone(parent);
      multiplyAffine(world, parent * MATRIX_FLOATS, local, offset, world, offset);
    }
    state[bone] = 2;
  };
  for (let bone = 0; bone < model.boneCount; bone++) evaluateWorldBone(bone);
  packScaledStagePose(model, world, skin, rows, stageScale);
  return rows;
}

/** Bind-pose bone rows in gameplay space.  `boneTops` maps a bone to the
    gameplay Y its mesh top should sit at (FoD replay heights).  The shift is
    applied after hierarchy + stage scale so parent JOBJ scale cannot leak. */
export function evaluateStagePose(
  model: ModelAsset, boneTops: ReadonlyMap<number, number>, output?: Float32Array, stageScale = 1,
): Float32Array {
  const rows = output ?? new Float32Array(model.boneCount * BONE_TEXTURE_FLOATS);
  const scratch = acquireStagePoseScratch(model.boneCount);
  const { local, world, skin, scale, rotation, translation, state } = scratch;
  local.set(model.boneBase);
  for (let bone = 0; bone < model.boneCount; bone++) decomposeBase(model, bone, scale, rotation, translation);
  state.fill(0);
  const evaluateWorldBone = (bone: number): void => {
    if (state[bone] === 2) return;
    if (state[bone] === 1) return;
    state[bone] = 1;
    const parent = model.boneParents[bone];
    const offset = bone * MATRIX_FLOATS;
    if (parent === 0xffff) {
      for (let index = 0; index < MATRIX_FLOATS; index++) world[offset + index] = local[offset + index];
    } else {
      evaluateWorldBone(parent);
      multiplyAffine(world, parent * MATRIX_FLOATS, local, offset, world, offset);
    }
    state[bone] = 2;
  };
  for (let bone = 0; bone < model.boneCount; bone++) evaluateWorldBone(bone);
  packScaledStagePose(model, world, skin, rows, stageScale, boneTops);
  return rows;
}
