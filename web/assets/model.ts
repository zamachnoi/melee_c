import { BinaryReader, boundedCount } from './binary.js';

export const ASSET_MAGIC = 0x4d444c00;
export const ASSET_SCHEMA = 4;

const LIMITS = Object.freeze({
  bones: 1024,
  vertices: 2_000_000,
  indices: 6_000_000,
  primitiveGroups: 100_000,
  phongs: 100_000,
  textures: 4096,
  textureDimension: 8192,
  textureBytes: 256 * 1024 * 1024,
});

export function parseModelFromReader(reader: BinaryReader, label = 'model') {
  const magic = reader.u32();
  if (magic !== ASSET_MAGIC) throw new Error(`${label}: bad magic 0x${magic.toString(16)}`);
  const schema = reader.u32();
  if (schema !== ASSET_SCHEMA) throw new Error(`${label}: unsupported asset schema ${schema}`);

  const boneCount = boundedCount(reader.u32(), LIMITS.bones, `${label} bones`);
  const vertexCount = boundedCount(reader.u32(), LIMITS.vertices, `${label} vertices`);
  const indexCount = boundedCount(reader.u32(), LIMITS.indices, `${label} indices`);
  const primitiveGroupCount = boundedCount(reader.u32(), LIMITS.primitiveGroups, `${label} primitive groups`);
  const phongCount = boundedCount(reader.u32(), LIMITS.phongs, `${label} phongs`);
  const textureCount = boundedCount(reader.u32(), LIMITS.textures, `${label} textures`);

  const boneParents = new Uint16Array(boneCount);
  const bonePrimitiveStart = new Uint16Array(boneCount);
  const bonePrimitiveLength = new Uint16Array(boneCount);
  const boneFlags = new Uint32Array(boneCount);
  const boneBase = new Float32Array(boneCount * 12);
  const boneInverseWorld = new Float32Array(boneCount * 12);
  for (let i = 0; i < boneCount; i++) {
    boneParents[i] = reader.u16();
    bonePrimitiveStart[i] = reader.u16();
    bonePrimitiveLength[i] = reader.u16();
    reader.u16();
    boneFlags[i] = reader.u32();
    for (let j = 0; j < 12; j++) boneBase[i * 12 + j] = reader.f32();
    for (let j = 0; j < 12; j++) boneInverseWorld[i * 12 + j] = reader.f32();
  }

  const positions = new Float32Array(vertexCount * 3);
  const normals = new Float32Array(vertexCount * 3);
  const uvs = new Float32Array(vertexCount * 2);
  const colors = new Uint8Array(vertexCount * 4);
  const weights = new Float32Array(vertexCount * 4);
  const boneIndices = new Uint16Array(vertexCount * 4);
  for (let i = 0; i < vertexCount; i++) {
    for (let j = 0; j < 3; j++) positions[i * 3 + j] = reader.f32();
    for (let j = 0; j < 3; j++) normals[i * 3 + j] = reader.f32();
    for (let j = 0; j < 2; j++) uvs[i * 2 + j] = reader.f32();
    colors.set(reader.bytes(4), i * 4);
    for (let j = 0; j < 4; j++) weights[i * 4 + j] = reader.f32();
    for (let j = 0; j < 4; j++) boneIndices[i * 4 + j] = reader.u16();
  }

  const indices = new Uint16Array(indexCount);
  for (let i = 0; i < indexCount; i++) indices[i] = reader.u16();

  const primitiveGroups = [];
  for (let i = 0; i < primitiveGroupCount; i++) {
    const textureIndex = reader.i16();
    const indexStart = reader.u32();
    const indexLength = reader.u32();
    const materialFlags = reader.u32();
    const modelGroupIndex = reader.u8();
    reader.u8(); reader.u16();
    if (indexStart > indexCount || indexLength > indexCount - indexStart) {
      throw new RangeError(`${label}: primitive group ${i} has invalid index range`);
    }
    if (textureIndex < -1 || textureIndex >= textureCount) {
      throw new RangeError(`${label}: primitive group ${i} has invalid texture ${textureIndex}`);
    }
    primitiveGroups.push({ textureIndex, indexStart, indexLength, materialFlags, modelGroupIndex });
  }

  const phongs = [];
  for (let i = 0; i < phongCount; i++) {
    phongs.push({
      ambient: reader.bytes(4),
      diffuse: reader.bytes(4),
      specular: reader.bytes(4),
      alpha: reader.f32(),
      shininess: reader.f32(),
    });
  }

  let textureBytes = 0;
  const textures = [];
  for (let i = 0; i < textureCount; i++) {
    const width = boundedCount(reader.u16(), LIMITS.textureDimension, `${label} texture width`);
    const height = boundedCount(reader.u16(), LIMITS.textureDimension, `${label} texture height`);
    const format = reader.u32();
    const size = width * height * 4;
    textureBytes += size;
    if (!Number.isSafeInteger(size) || textureBytes > LIMITS.textureBytes) {
      throw new RangeError(`${label}: decoded textures exceed ${LIMITS.textureBytes} bytes`);
    }
    textures.push({ width, height, format, rgba: reader.bytes(size) });
  }

  return {
    schema, boneCount, vertexCount, indexCount,
    boneParents, bonePrimitiveStart, bonePrimitiveLength, boneFlags,
    boneBase, boneInverseWorld, positions, normals, uvs, colors, weights,
    boneIndices, indices, primitiveGroups, phongs, textures,
    decodedBytes: boneBase.byteLength + boneInverseWorld.byteLength +
      positions.byteLength + normals.byteLength + uvs.byteLength +
      colors.byteLength + weights.byteLength + boneIndices.byteLength +
      indices.byteLength + textureBytes,
  };
}

export function parseModel(buffer: ArrayBuffer) {
  const reader = new BinaryReader(buffer);
  const model = parseModelFromReader(reader);
  if (reader.offset !== buffer.byteLength) throw new Error(`model: ${buffer.byteLength - reader.offset} trailing bytes`);
  return model;
}

export type ModelAsset = ReturnType<typeof parseModel>;
