import type { ModelAsset } from '../assets/model.js';
import type { AnimationsAsset } from '../assets/anims.js';
import { applyAffine, MATRIX_FLOATS, multiplyAffine } from '../animation/pose.js';

const JOBJ_CLASSICAL_SCALE = 1 << 3;

/** HSD Maya static scale compensation (matches animation/pose.ts). */
function applyMayaSsc(matrix: Float32Array, offset: number, ps: Float32Array, psOffset: number): void {
  const ps0 = ps[psOffset], ps1 = ps[psOffset + 1], ps2 = ps[psOffset + 2];
  matrix[offset + 1] *= ps0 / ps1;
  matrix[offset + 2] *= ps0 / ps2;
  matrix[offset + 3] *= ps1 / ps0;
  matrix[offset + 5] *= ps1 / ps2;
  matrix[offset + 6] *= ps2 / ps0;
  matrix[offset + 7] *= ps2 / ps1;
}

/** Transform the bind pose to world once at upload time. */
export function transformBindPose(model: ModelAsset): Float32Array {
  const world = new Float32Array(model.boneCount * MATRIX_FLOATS);
  const local = new Float32Array(model.boneCount * MATRIX_FLOATS);
  const state = new Uint8Array(model.boneCount);
  const scale = new Float32Array(model.boneCount * 3);
  const sscPs = new Float32Array(model.boneCount * 3);
  const sscAcc = new Float32Array(model.boneCount * 3);
  const columnScale = (o: number, c: number): number =>
    Math.hypot(model.boneBase[o + c * 3], model.boneBase[o + c * 3 + 1], model.boneBase[o + c * 3 + 2]);
  for (let bone = 0; bone < model.boneCount; bone++) {
    const o = bone * MATRIX_FLOATS;
    const so = bone * 3;
    scale[so] = columnScale(o, 0);
    scale[so + 1] = columnScale(o, 1);
    scale[so + 2] = columnScale(o, 2);
    const parent = model.boneParents[bone];
    if (parent === 0xffff) {
      sscPs[so] = 1; sscPs[so + 1] = 1; sscPs[so + 2] = 1;
    } else {
      const po = parent * 3;
      sscPs[so] = sscAcc[po]; sscPs[so + 1] = sscAcc[po + 1]; sscPs[so + 2] = sscAcc[po + 2];
    }
    if (model.boneFlags[bone] & JOBJ_CLASSICAL_SCALE) {
      sscAcc[so] = sscPs[so]; sscAcc[so + 1] = sscPs[so + 1]; sscAcc[so + 2] = sscPs[so + 2];
    } else {
      sscAcc[so] = scale[so] * sscPs[so];
      sscAcc[so + 1] = scale[so + 1] * sscPs[so + 1];
      sscAcc[so + 2] = scale[so + 2] * sscPs[so + 2];
    }
  }
  const evaluate = (bone: number): void => {
    if (state[bone] === 2) return;
    if (state[bone] === 1) throw new Error(`model: bone cycle at ${bone}`);
    state[bone] = 1;
    const parent = model.boneParents[bone];
    const offset = bone * MATRIX_FLOATS;
    local.set(model.boneBase.subarray(offset, offset + MATRIX_FLOATS), offset);
    applyMayaSsc(local, offset, sscPs, bone * 3);
    if (parent !== 0xffff) {
      if (parent >= model.boneCount) throw new Error(`model: bone ${bone} has invalid parent ${parent}`);
      evaluate(parent);
      multiplyAffine(world, parent * MATRIX_FLOATS, local, offset, world, offset);
    } else {
      for (let index = 0; index < MATRIX_FLOATS; index++) world[offset + index] = local[offset + index];
    }
    state[bone] = 2;
  };
  for (let bone = 0; bone < model.boneCount; bone++) evaluate(bone);

  const positions = new Float32Array(model.positions.length);
  const point = new Float32Array(3);
  const skin = new Float32Array(MATRIX_FLOATS);
  for (let vertex = 0; vertex < model.vertexCount; vertex++) {
    const po = vertex * 3;
    const wo = vertex * 4;
    const x = model.positions[po];
    const y = model.positions[po + 1];
    const z = model.positions[po + 2];
    let influences = 0;
    for (let k = 0; k < 4; k++) {
      if (model.weights[wo + k] > 0.0001 && model.boneIndices[wo + k] < model.boneCount) influences++;
    }
    let ax = 0, ay = 0, az = 0, totalWeight = 0;
    for (let k = 0; k < 4; k++) {
      const weight = model.weights[wo + k];
      const bone = model.boneIndices[wo + k];
      if (weight <= 0.0001 || bone >= model.boneCount) continue;
      if (influences > 1) {
        multiplyAffine(world, bone * MATRIX_FLOATS, model.boneInverseWorld, bone * MATRIX_FLOATS, skin, 0);
        applyAffine(skin, 0, x, y, z, point);
      } else {
        applyAffine(world, bone * MATRIX_FLOATS, x, y, z, point);
      }
      ax += point[0] * weight;
      ay += point[1] * weight;
      az += point[2] * weight;
      totalWeight += weight;
    }
    if (totalWeight > 0) {
      positions[po] = ax / totalWeight;
      positions[po + 1] = ay / totalWeight;
      positions[po + 2] = az / totalWeight;
    } else {
      positions[po] = x;
      positions[po + 1] = y;
      positions[po + 2] = z;
    }
  }
  return positions;
}

export function positionBounds(positions: Float32Array): [number, number, number, number, number, number] {
  if (positions.length < 3) return [Infinity, Infinity, Infinity, -Infinity, -Infinity, -Infinity];
  let minX = positions[0], minY = positions[1], minZ = positions[2];
  let maxX = minX, maxY = minY, maxZ = minZ;
  for (let i = 3; i < positions.length; i += 3) {
    minX = Math.min(minX, positions[i]); maxX = Math.max(maxX, positions[i]);
    minY = Math.min(minY, positions[i + 1]); maxY = Math.max(maxY, positions[i + 1]);
    minZ = Math.min(minZ, positions[i + 2]); maxZ = Math.max(maxZ, positions[i + 2]);
  }
  return [minX, minY, minZ, maxX, maxY, maxZ];
}

export function isAcceptedFdSection(model: ModelAsset): boolean {
  if (!model.vertexCount) return false;
  const bounds = positionBounds(transformBindPose(model));
  return bounds[0] >= -100 && bounds[3] <= 100 && bounds[4] <= 5 && bounds[4] - bounds[1] >= 1;
}

export type StageSectionLayer = 'skip' | 'background' | 'playable';

const JOBJ_HIDDEN = 1 << 4;

/** Which draw list a map_head section belongs to, decided generically by
    geometry rather than a per-stage spawn table (noclip renders every gobj;
    this only splits skybox/backdrop planes from the playable arena for draw
    order).  Sections whose root JOBJ starts hidden are skipped entirely. */
export function classifyStageSection(stageId: number, mapId: number, model: ModelAsset): StageSectionLayer {
  if (!model.vertexCount) return 'skip';
  if (model.boneCount && (model.boneFlags[0] & JOBJ_HIDDEN)) return 'skip';
  if (isStageSection(model)) return 'playable';
  if (isBackgroundSection(model)) return 'background';
  return 'skip';
}

/** A stage section animates when its map_head AnimJoint clip decodes to real
    joint tracks.  noclip plays every gobj's anim[0], so we do the same. */
export function stageMapAnimates(action: AnimationsAsset['actions'][number] | undefined): boolean {
  return Boolean(action?.joints.length);
}

/** A stage section that belongs to the playable arena rather than the skybox.
    Size heuristic used only for stages without a decomp map_id table. */
export function isStageSection(model: ModelAsset): boolean {
  if (!model.vertexCount) return false;
  const bounds = positionBounds(transformBindPose(model));
  const width = bounds[3] - bounds[0];
  const height = bounds[4] - bounds[1];
  const depth = bounds[5] - bounds[2];
  const shortest = Math.min(width, height, depth);
  const longest = Math.max(width, height, depth);
  if (height < 1) return false;
  if (width > 2000 || depth > 2000 || height > 2000) return false;
  if (shortest < 1 && longest > 400) return false;
  if (shortest > 0 && longest / shortest < 2 && width > 400) return false;
  return true;
}

/** Skybox / audience / painted-backdrop geometry that should render behind the
    playable arena.  Size heuristic used only for stages without a decomp table. */
export function isBackgroundSection(model: ModelAsset): boolean {
  if (!model.vertexCount || isStageSection(model)) return false;
  const bounds = positionBounds(transformBindPose(model));
  const width = bounds[3] - bounds[0];
  const height = bounds[4] - bounds[1];
  const depth = bounds[5] - bounds[2];
  const shortest = Math.min(width, height, depth);
  const longest = Math.max(width, height, depth);
  if (shortest < 1 && longest < 400) return false;
  return true;
}
