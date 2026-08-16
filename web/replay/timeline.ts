export const TIMELINE_SCHEMA = 1;
export const LIVE_PROTOCOL = 1;
const MAGIC = 0x52504c32;
const HEADER_SIZE = 64;
const PLAYER_SIZE = 64;
const SLOT_SIZE = 56;

export interface TimelinePlayer {
  port: number;
  externalCharacterId: number;
  costume: number;
  startingStocks: number;
  team: number;
  type: number;
  name: string;
  connectCode: string;
}

export interface TimelineSlot {
  port: number;
  follower: boolean;
  active: boolean;
  presence: Uint8Array;
  character: Uint8Array;
  animationIndex: Uint32Array;
  actionState: Uint16Array;
  animationFrame: Float32Array;
  x: Float32Array;
  y: Float32Array;
  facing: Float32Array;
  percent: Float32Array;
  shield: Float32Array;
  stocks: Uint8Array;
  stateFlags: Uint8Array;
}

export interface TimelineFrame {
  frame: number;
  character: number;
  animationIndex: number;
  actionState: number;
  animationFrame: number;
  x: number;
  y: number;
  facing: number;
  percent: number;
  shield: number;
  stocks: number;
  flags: number;
}

export interface TimelineItem {
  frame: number; spawnId: number; typeId: number; state: number; owner: number;
  facing: number; velocityX: number; velocityY: number; x: number; y: number;
  damage: number; instanceId: number;
}

export interface TimelineStageEvent {
  frame: number; kind: number; index: number; value0: number; value1: number;
}

export interface TimelineCamera {
  x: Float32Array; y: Float32Array; zoom: Float32Array;
}

export interface Timeline {
  schema: number; flags: number; startFrame: number; endFrame: number;
  stageId: number; frameCount: number; assetSchema: number; liveProtocol: number;
  players: TimelinePlayer[]; slots: TimelineSlot[]; items: TimelineItem[];
  stageEvents: TimelineStageEvent[]; camera: TimelineCamera | null; byteLength: number;
  frame(slotIndex: number, frame: number): TimelineFrame | null;
}

function checkedRange(view: DataView, offset: number, size: number, end: number, label: string): void {
  if (!Number.isSafeInteger(offset) || !Number.isSafeInteger(size) || offset < HEADER_SIZE ||
      size < 0 || offset > end || size > end - offset || end > view.byteLength) {
    throw new RangeError(`${label}: invalid range ${offset}+${size}`);
  }
}

function u8Array(view: DataView, offset: number, count: number, end: number): Uint8Array {
  checkedRange(view, offset, count, end, 'u8 field');
  return new Uint8Array(view.buffer.slice(offset, offset + count));
}

function u16Array(view: DataView, offset: number, count: number, end: number): Uint16Array {
  checkedRange(view, offset, count * 2, end, 'u16 field');
  const out = new Uint16Array(count);
  for (let i = 0; i < count; i++) out[i] = view.getUint16(offset + i * 2, false);
  return out;
}

function u32Array(view: DataView, offset: number, count: number, end: number): Uint32Array {
  checkedRange(view, offset, count * 4, end, 'u32 field');
  const out = new Uint32Array(count);
  for (let i = 0; i < count; i++) out[i] = view.getUint32(offset + i * 4, false);
  return out;
}

function f32Array(view: DataView, offset: number, count: number, end: number): Float32Array {
  checkedRange(view, offset, count * 4, end, 'f32 field');
  const out = new Float32Array(count);
  for (let i = 0; i < count; i++) out[i] = view.getFloat32(offset + i * 4, false);
  return out;
}

function cstring(view: DataView, offset: number, size: number): string {
  const bytes = new Uint8Array(view.buffer, offset, size);
  const zero = bytes.indexOf(0);
  return new TextDecoder().decode(zero < 0 ? bytes : bytes.subarray(0, zero));
}

export function parseTimeline(buffer: ArrayBuffer): Timeline {
  if (!(buffer instanceof ArrayBuffer) || buffer.byteLength < HEADER_SIZE) throw new Error('timeline: truncated header');
  const view = new DataView(buffer);
  if (view.getUint32(0, false) !== MAGIC) throw new Error('timeline: bad magic');
  const schema = view.getUint16(4, false);
  if (schema !== TIMELINE_SCHEMA) throw new Error(`timeline: unsupported schema ${schema}`);
  const flags = view.getUint16(6, false);
  const startFrame = view.getInt32(8, false);
  const endFrame = view.getInt32(12, false);
  const stageId = view.getUint16(16, false);
  const slotCount = view.getUint8(18);
  const playerCount = view.getUint8(19);
  const frameCount = view.getUint32(20, false);
  const playersOffset = view.getUint32(24, false);
  const slotsOffset = view.getUint32(28, false);
  const itemsOffset = view.getUint32(32, false);
  const itemCount = view.getUint32(36, false);
  const eventsOffset = view.getUint32(40, false);
  const eventCount = view.getUint32(44, false);
  const camerasOffset = view.getUint32(48, false);
  const endOffset = view.getUint32(52, false);
  const assetSchema = view.getUint16(56, false);
  const liveProtocol = view.getUint16(58, false);
  if (slotCount !== 8 || playerCount > 4 || endFrame < startFrame ||
      frameCount !== endFrame - startFrame + 1 || endOffset > buffer.byteLength) {
    throw new Error('timeline: inconsistent header');
  }
  checkedRange(view, playersOffset, playerCount * PLAYER_SIZE, endOffset, 'players');
  checkedRange(view, slotsOffset, slotCount * SLOT_SIZE, endOffset, 'slots');
  const players: TimelinePlayer[] = [];
  for (let i = 0; i < playerCount; i++) {
    const p = playersOffset + i * PLAYER_SIZE;
    players.push({
      port: view.getUint8(p), externalCharacterId: view.getUint8(p + 1),
      costume: view.getUint8(p + 2), startingStocks: view.getUint8(p + 3),
      team: view.getUint8(p + 4), type: view.getUint8(p + 5),
      name: cstring(view, p + 8, 32), connectCode: cstring(view, p + 40, 16),
    });
  }
  const slots: TimelineSlot[] = [];
  for (let i = 0; i < slotCount; i++) {
    const d = slotsOffset + i * SLOT_SIZE;
    const offsets = Array.from({ length: 13 }, (_, j) => view.getUint32(d + 4 + j * 4, false));
    const slot: TimelineSlot = {
      port: view.getUint8(d), follower: view.getUint8(d + 1) !== 0,
      active: view.getUint8(d + 2) !== 0,
      presence: u8Array(view, offsets[0], frameCount, endOffset),
      character: u8Array(view, offsets[1], frameCount, endOffset),
      animationIndex: u32Array(view, offsets[2], frameCount, endOffset),
      actionState: u16Array(view, offsets[3], frameCount, endOffset),
      animationFrame: f32Array(view, offsets[4], frameCount, endOffset),
      x: f32Array(view, offsets[5], frameCount, endOffset),
      y: f32Array(view, offsets[6], frameCount, endOffset),
      facing: f32Array(view, offsets[7], frameCount, endOffset),
      percent: f32Array(view, offsets[8], frameCount, endOffset),
      shield: f32Array(view, offsets[9], frameCount, endOffset),
      stocks: u8Array(view, offsets[10], frameCount, endOffset),
      stateFlags: u8Array(view, offsets[11], frameCount, endOffset),
    };
    if (offsets[12] > endOffset) throw new RangeError('timeline: slot end outside payload');
    slots.push(slot);
  }
  checkedRange(view, itemsOffset, itemCount * 40, endOffset, 'items');
  const items: TimelineItem[] = [];
  for (let i = 0; i < itemCount; i++) {
    const p = itemsOffset + i * 40;
    items.push({ frame: view.getInt32(p, false), spawnId: view.getUint32(p + 4, false),
      typeId: view.getUint16(p + 8, false), state: view.getUint8(p + 10), owner: view.getInt8(p + 11),
      facing: view.getFloat32(p + 12, false), velocityX: view.getFloat32(p + 16, false),
      velocityY: view.getFloat32(p + 20, false), x: view.getFloat32(p + 24, false),
      y: view.getFloat32(p + 28, false), damage: view.getUint16(p + 32, false),
      instanceId: view.getUint16(p + 34, false) });
  }
  checkedRange(view, eventsOffset, eventCount * 16, endOffset, 'stage events');
  const stageEvents: TimelineStageEvent[] = [];
  for (let i = 0; i < eventCount; i++) {
    const p = eventsOffset + i * 16;
    const kind = view.getUint8(p + 4);
    stageEvents.push({ frame: view.getInt32(p, false), kind, index: view.getUint8(p + 5),
      value0: kind === 1 ? view.getFloat32(p + 8, false) : view.getUint32(p + 8, false),
      value1: view.getUint32(p + 12, false) });
  }
  let camera: TimelineCamera | null = null;
  if (camerasOffset) {
    checkedRange(view, camerasOffset, 16, endOffset, 'camera header');
    camera = {
      x: f32Array(view, view.getUint32(camerasOffset, false), frameCount, endOffset),
      y: f32Array(view, view.getUint32(camerasOffset + 4, false), frameCount, endOffset),
      zoom: f32Array(view, view.getUint32(camerasOffset + 8, false), frameCount, endOffset),
    };
  }
  return {
    schema, flags, startFrame, endFrame, stageId, frameCount, assetSchema,
    liveProtocol, players, slots, items, stageEvents, camera,
    byteLength: endOffset,
    frame(slotIndex: number, frame: number): TimelineFrame | null {
      const index = frame - startFrame;
      if (slotIndex < 0 || slotIndex >= slots.length || index < 0 || index >= frameCount || !slots[slotIndex].presence[index]) return null;
      const s = slots[slotIndex];
      return { frame, character: s.character[index], animationIndex: s.animationIndex[index],
        actionState: s.actionState[index], animationFrame: s.animationFrame[index],
        x: s.x[index], y: s.y[index], facing: s.facing[index], percent: s.percent[index],
        shield: s.shield[index], stocks: s.stocks[index], flags: s.stateFlags[index] };
    },
  };
}

/** Parse with a simple timing/size measurement suitable for diagnostics. */
export function measureTimeline(buffer: ArrayBuffer) {
  const start = performance.now();
  const timeline = parseTimeline(buffer);
  return { timeline, wireBytes: buffer.byteLength, parseMilliseconds: performance.now() - start };
}
