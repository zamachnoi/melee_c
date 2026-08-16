import { BinaryReader, boundedCount } from './binary.js';
import { ASSET_MAGIC, ASSET_SCHEMA, parseModelFromReader } from './model.js';

export function parseStage(buffer: ArrayBuffer) {
  const reader = new BinaryReader(buffer);
  const magic = reader.u32();
  if (magic !== ASSET_MAGIC) throw new Error('stage: bad magic');
  const schema = reader.u32();
  if (schema !== ASSET_SCHEMA) throw new Error(`stage: unsupported asset schema ${schema}`);
  const scale = reader.f32();
  const cameraPosition = new Float32Array([reader.f32(), reader.f32(), reader.f32()]);
  const cameraFov = reader.f32();
  const cameraVerticalAngle = reader.f32();
  const cameraHorizontalAngle = reader.f32();
  const sectionCount = boundedCount(reader.u32(), 1024, 'stage sections');
  const lightCount = boundedCount(reader.u32(), 4096, 'stage lights');
  const sections = [];
  for (let i = 0; i < sectionCount; i++) sections.push(parseModelFromReader(reader, `stage section ${i}`));
  const lights = [];
  for (let i = 0; i < lightCount; i++) {
    lights.push({
      kind: reader.u8(), flags: reader.u8(), color: reader.bytes(4),
      position: new Float32Array([reader.f32(), reader.f32(), reader.f32()]),
      direction: new Float32Array([reader.f32(), reader.f32(), reader.f32()]),
      attenuation: new Float32Array([reader.f32(), reader.f32(), reader.f32(), reader.f32(), reader.f32(), reader.f32()]),
    });
  }
  if (reader.offset !== buffer.byteLength) throw new Error(`stage: ${buffer.byteLength - reader.offset} trailing bytes`);
  return { schema, scale, cameraPosition, cameraFov, cameraVerticalAngle, cameraHorizontalAngle, sections, lights };
}
