import type { ModelAsset } from '../assets/model.js';
import { applyAffine, MATRIX_FLOATS, multiplyAffine } from '../animation/pose.js';

/** Match render.c's bind-pose rigid/weighted vertex rules once at upload time. */
export function transformBindPose(model: ModelAsset): Float32Array {
  const world = new Float32Array(model.boneCount * MATRIX_FLOATS);
  const state = new Uint8Array(model.boneCount);
  const evaluate = (bone: number): void => {
    if (state[bone] === 2) return;
    if (state[bone] === 1) throw new Error(`model: bone cycle at ${bone}`);
    state[bone] = 1;
    const parent = model.boneParents[bone];
    const offset = bone * MATRIX_FLOATS;
    if (parent === 0xffff) {
      world.set(model.boneBase.subarray(offset, offset + MATRIX_FLOATS), offset);
    } else {
      if (parent >= model.boneCount) throw new Error(`model: bone ${bone} has invalid parent ${parent}`);
      evaluate(parent);
      multiplyAffine(world, parent * MATRIX_FLOATS, model.boneBase, offset, world, offset);
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

/** A stage section that belongs to the playable arena rather than the skybox.
    Keeps platform/structure sections (Battlefield sec 6, FoD platforms,
    Yoshi's Story's main island) and drops huge background domes. */
export function isStageSection(model: ModelAsset): boolean {
  if (!model.vertexCount) return false;
  const bounds = positionBounds(transformBindPose(model));
  const width = bounds[3] - bounds[0];
  const height = bounds[4] - bounds[1];
  const depth = bounds[5] - bounds[2];
  if (height < 1) return false;
  if (width > 2000 || depth > 2000 || height > 2000) return false;
  const shortest = Math.min(width, height, depth);
  const longest = Math.max(width, height, depth);
  if (shortest > 0 && longest / shortest < 2 && width > 400) return false;
  return true;
}
