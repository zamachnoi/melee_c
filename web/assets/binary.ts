/** Strict big-endian reader shared by extracted-asset parsers. */
export class BinaryReader {
  readonly buffer: ArrayBuffer;
  readonly view: DataView;
  offset = 0;

  constructor(buffer: ArrayBuffer) {
    if (!(buffer instanceof ArrayBuffer)) throw new TypeError('expected ArrayBuffer');
    this.buffer = buffer;
    this.view = new DataView(buffer);
    this.offset = 0;
  }

  require(size: number): void {
    if (!Number.isSafeInteger(size) || size < 0 || this.offset + size > this.view.byteLength) {
      throw new RangeError(`truncated binary at ${this.offset} (need ${size} bytes)`);
    }
  }

  u8(): number { this.require(1); return this.view.getUint8(this.offset++); }
  u16(): number { this.require(2); const v = this.view.getUint16(this.offset, false); this.offset += 2; return v; }
  i16(): number { this.require(2); const v = this.view.getInt16(this.offset, false); this.offset += 2; return v; }
  u32(): number { this.require(4); const v = this.view.getUint32(this.offset, false); this.offset += 4; return v; }
  f32(): number { this.require(4); const v = this.view.getFloat32(this.offset, false); this.offset += 4; return v; }

  bytes(size: number): Uint8Array {
    this.require(size);
    const out = new Uint8Array(size);
    out.set(new Uint8Array(this.buffer, this.offset, size));
    this.offset += size;
    return out;
  }
}

export function boundedCount(value: number, maximum: number, label: string): number {
  if (!Number.isSafeInteger(value) || value < 0 || value > maximum) {
    throw new RangeError(`${label} count ${value} exceeds ${maximum}`);
  }
  return value;
}

export function decodeCString(bytes: Uint8Array): string {
  const end = bytes.indexOf(0);
  return new TextDecoder().decode(end < 0 ? bytes : bytes.subarray(0, end));
}
