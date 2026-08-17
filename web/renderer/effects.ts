/** Melee overlay effects: shield bubble, shine, firefox, and lasers. */

import type { ModelAsset } from '../assets/model.js';

export const EffectKind = {
  Shield: 0,
  Shine: 1,
  Fire: 2,
  Laser: 3,
} as const;

export type EffectKindId = (typeof EffectKind)[keyof typeof EffectKind];

export const PORT_SHIELD_COLOR = [
  [1, 0.22, 0.28],
  [0.22, 0.48, 1],
  [1, 0.86, 0.16],
  [0.22, 0.86, 0.38],
] as const;

/** Full shield_size is 60; Melee's bubble is about 12.5 world units across a radius. */
export const SHIELD_RADIUS_AT_FULL = 12.5;
export const SHIELD_Y_OFFSET = 8;

export function portShieldColor(port: number): readonly [number, number, number] {
  return PORT_SHIELD_COLOR[((port % 4) + 4) % 4] as unknown as [number, number, number];
}

export function shieldRadius(size: number): number {
  return Math.max(2.5, (Math.min(60, Math.max(0, size)) / 60) * SHIELD_RADIUS_AT_FULL);
}

export const CHAR_FOX = 1;
export const CHAR_FALCO = 22;

export const SHINE_RADIUS = 10;
export const FIRE_RADIUS = 11;
export const LASER_THICKNESS = 1.15;
export const SHINE_COLOR = [0.32, 1, 0.94] as const;
export const FIRE_COLOR = [1, 0.42, 0.08] as const;
export const FOX_LASER_COLOR = [1, 0.3, 0.25] as const;
export const FALCO_LASER_COLOR = [0.31, 0.73, 1] as const;

export function isShieldState(actionState: number, shield: number): boolean {
  return actionState >= 178 && actionState <= 182 && shield > 0;
}

export function isSpacie(characterId: number): boolean {
  return characterId === CHAR_FOX || characterId === CHAR_FALCO;
}

export function isShineAction(name: string | null | undefined): boolean {
  return Boolean(name && /Special(?:Air)?Lw/i.test(name));
}

export function isFirefoxAction(name: string | null | undefined): boolean {
  return Boolean(name && /Special(?:Air)?Hi/i.test(name));
}

export function isFoxLaser(typeId: number): boolean {
  return typeId === 0x36;
}

export function isFalcoLaser(typeId: number): boolean {
  return typeId === 0x37;
}

export function isFoxIllusion(typeId: number): boolean {
  return typeId === 0x38;
}

export function isFalcoPhantasm(typeId: number): boolean {
  return typeId === 0x39;
}

export function laserLength(velocityX: number, velocityY: number): number {
  return Math.max(10, Math.hypot(velocityX, velocityY) * 1.25);
}

export interface EffectCatalog {
  schema: number;
  aliases: Record<string, string>;
  items: Record<string, string>;
  gfx: Record<string, string>;
}

export interface EffectModelBank {
  byAlias: Readonly<Record<string, ModelAsset>>;
  byItem: Readonly<Record<number, ModelAsset>>;
}

const EFFECT_FILE = /^[a-z0-9][a-z0-9._-]*\.model$/;

function catalogStringMap(value: unknown, label: string): Record<string, string> {
  if (!value || typeof value !== 'object' || Array.isArray(value)) {
    throw new Error(`effects catalog ${label} is not an object`);
  }
  const out: Record<string, string> = {};
  for (const [key, file] of Object.entries(value as Record<string, unknown>)) {
    if (typeof file !== 'string' || !EFFECT_FILE.test(file)) continue;
    out[key] = file;
  }
  return out;
}

export function parseEffectCatalog(value: unknown): EffectCatalog {
  if (!value || typeof value !== 'object' || Array.isArray(value)) {
    throw new Error('effects catalog is not an object');
  }
  const raw = value as Record<string, unknown>;
  if (raw.schema !== 4) throw new Error(`effects catalog schema ${String(raw.schema)}`);
  return {
    schema: 4,
    aliases: catalogStringMap(raw.aliases, 'aliases'),
    items: catalogStringMap(raw.items, 'items'),
    gfx: catalogStringMap(raw.gfx, 'gfx'),
  };
}

export function effectAssetUrl(file: string): string {
  return `/assets/v5/effects/${file}?v=shield-mirror`;
}

export const EFFECT_ALIAS_KEYS = [
  'shield', 'powershield', 'shine', 'shine-start', 'shine-hit',
  'firefox', 'firefox-charge',
] as const;

export function xyRadius(positions: Float32Array): number {
  let max = 0;
  for (let i = 0; i < positions.length; i += 3) {
    max = Math.max(max, Math.hypot(positions[i], positions[i + 1]));
  }
  return max;
}

/** Axis-aligned half-extent; shield billboards scale this to the collision radius. */
export function xyExtent(positions: Float32Array): number {
  let max = 0;
  for (let i = 0; i < positions.length; i += 3) {
    max = Math.max(max, Math.abs(positions[i]), Math.abs(positions[i + 1]));
  }
  return max;
}

/** Scale a DAT mesh so its XY radius matches a gameplay size, unless it is already close. */
export function fitEffectScale(modelRadius: number, targetRadius: number): number {
  if (!(modelRadius > 0.05) || !(targetRadius > 0)) return 1;
  const ratio = targetRadius / modelRadius;
  return ratio > 0.5 && ratio < 2.5 ? 1 : ratio;
}

export interface GeneratedMesh {
  positions: Float32Array;
  uvs: Float32Array;
  indices: Uint16Array;
}

export function createUvSphere(rings = 20, segments = 32): GeneratedMesh {
  const positions: number[] = [];
  const uvs: number[] = [];
  for (let ring = 0; ring <= rings; ring++) {
    const v = ring / rings;
    const phi = v * Math.PI;
    const y = Math.cos(phi);
    const radius = Math.sin(phi);
    for (let segment = 0; segment <= segments; segment++) {
      const u = segment / segments;
      const theta = u * Math.PI * 2;
      positions.push(radius * Math.cos(theta), y, radius * Math.sin(theta));
      uvs.push(u, v);
    }
  }
  const indices: number[] = [];
  const stride = segments + 1;
  for (let ring = 0; ring < rings; ring++) {
    for (let segment = 0; segment < segments; segment++) {
      const a = ring * stride + segment;
      const b = a + stride;
      indices.push(a, b, a + 1, a + 1, b, b + 1);
    }
  }
  return {
    positions: new Float32Array(positions),
    uvs: new Float32Array(uvs),
    indices: new Uint16Array(indices),
  };
}

/** Flat hexagonal prism used for Fox/Falco shine. */
export function createHexPrism(depth = 0.35): GeneratedMesh {
  const positions: number[] = [];
  const uvs: number[] = [];
  const indices: number[] = [];
  const push = (x: number, y: number, z: number, u: number, v: number): void => {
    positions.push(x, y, z);
    uvs.push(u, v);
  };
  for (let side = 0; side < 2; side++) {
    const z = side === 0 ? depth : -depth;
    push(0, 0, z, 0.5, 0.5);
    for (let i = 0; i < 6; i++) {
      const angle = (Math.PI / 3) * i - Math.PI / 6;
      const x = Math.cos(angle), y = Math.sin(angle);
      push(x, y, z, x * 0.5 + 0.5, y * 0.5 + 0.5);
    }
    const center = side * 7;
    for (let i = 0; i < 6; i++) {
      const a = center + 1 + i;
      const b = center + 1 + ((i + 1) % 6);
      if (side === 0) indices.push(center, a, b);
      else indices.push(center, b, a);
    }
  }
  for (let i = 0; i < 6; i++) {
    const a = 1 + i, b = 1 + ((i + 1) % 6);
    const c = 8 + i, d = 8 + ((i + 1) % 6);
    indices.push(a, c, b, b, c, d);
  }
  return {
    positions: new Float32Array(positions),
    uvs: new Float32Array(uvs),
    indices: new Uint16Array(indices),
  };
}

export function createUnitCylinder(segments = 12): GeneratedMesh {
  const positions: number[] = [];
  const uvs: number[] = [];
  const indices: number[] = [];
  for (let end = 0; end <= 1; end++) {
    const x = end === 0 ? -0.5 : 0.5;
    for (let i = 0; i <= segments; i++) {
      const u = i / segments;
      const angle = u * Math.PI * 2;
      positions.push(x, Math.cos(angle), Math.sin(angle));
      uvs.push(u, end);
    }
  }
  const stride = segments + 1;
  for (let i = 0; i < segments; i++) {
    const a = i, b = i + 1, c = stride + i, d = stride + i + 1;
    indices.push(a, c, b, b, c, d);
  }
  return {
    positions: new Float32Array(positions),
    uvs: new Float32Array(uvs),
    indices: new Uint16Array(indices),
  };
}
