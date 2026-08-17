import type { Timeline, TimelineItem, TimelineStageEvent } from './timeline.js';

export const STAGE_EVENT_FOD = 1;
export const STAGE_EVENT_WHISPY = 2;
export const STAGE_EVENT_STADIUM = 3;

const STADIUM_ON_FIELD_EVENTS = new Set([0, 5, 6]);

/** The transformation that is on the field, ignoring jumbotron preview events. */
export function stadiumFieldType(event: number, type: number, previous: number): number {
  return STADIUM_ON_FIELD_EVENTS.has(event) ? type : previous;
}

function fillFrameRanges<T extends { frame: number }>(
  records: readonly T[], startFrame: number, frameCount: number,
  starts: Uint32Array, ends: Uint32Array, label: string,
): void {
  let cursor = 0;
  let previous = -Infinity;
  for (let record = 0; record < records.length; record++) {
    if (records[record].frame < previous) throw new Error(`${label} records are not sorted by frame`);
    previous = records[record].frame;
  }
  for (let index = 0; index < frameCount; index++) {
    const frame = startFrame + index;
    while (cursor < records.length && records[cursor].frame < frame) cursor++;
    starts[index] = cursor;
    while (cursor < records.length && records[cursor].frame === frame) cursor++;
    ends[index] = cursor;
  }
}

/** Allocation-free frame lookup and persistent dynamic-stage state. */
export class ReplaySceneIndex {
  readonly itemStarts: Uint32Array;
  readonly itemEnds: Uint32Array;
  readonly stageEventStarts: Uint32Array;
  readonly stageEventEnds: Uint32Array;
  readonly fodLeft: Float32Array;
  readonly fodRight: Float32Array;
  readonly whispyDirection: Int8Array;
  readonly stadiumEvent: Int16Array;
  readonly stadiumType: Int16Array;
  readonly stadiumVisibleType: Int16Array;

  constructor(readonly timeline: Timeline) {
    const count = timeline.frameCount;
    this.itemStarts = new Uint32Array(count);
    this.itemEnds = new Uint32Array(count);
    this.stageEventStarts = new Uint32Array(count);
    this.stageEventEnds = new Uint32Array(count);
    fillFrameRanges<TimelineItem>(timeline.items, timeline.startFrame, count,
      this.itemStarts, this.itemEnds, 'item');
    fillFrameRanges<TimelineStageEvent>(timeline.stageEvents, timeline.startFrame, count,
      this.stageEventStarts, this.stageEventEnds, 'stage event');

    this.fodLeft = new Float32Array(count);
    this.fodRight = new Float32Array(count);
    this.fodLeft.fill(Number.NaN);
    this.fodRight.fill(Number.NaN);
    this.whispyDirection = new Int8Array(count);
    this.stadiumEvent = new Int16Array(count);
    this.stadiumType = new Int16Array(count);
    this.stadiumVisibleType = new Int16Array(count);
    this.whispyDirection.fill(-1);
    this.stadiumEvent.fill(-1);
    this.stadiumType.fill(-1);
    this.stadiumVisibleType.fill(-1);

    let left = Number.NaN, right = Number.NaN;
    let whispy = -1, stadiumEvent = -1, stadiumType = -1, stadiumVisibleType = -1;
    for (let index = 0; index < count; index++) {
      for (let event = this.stageEventStarts[index]; event < this.stageEventEnds[index]; event++) {
        const value = timeline.stageEvents[event];
        if (value.kind === STAGE_EVENT_FOD) {
          /* Slippi 0x3F: platform 0 = right, 1 = left. */
          if (value.index === 0) right = value.value0;
          else if (value.index === 1) left = value.value0;
        } else if (value.kind === STAGE_EVENT_WHISPY) whispy = value.value0;
        else if (value.kind === STAGE_EVENT_STADIUM) {
          stadiumEvent = value.value0;
          stadiumType = value.value1;
          stadiumVisibleType = stadiumFieldType(stadiumEvent, stadiumType, stadiumVisibleType);
        }
      }
      this.fodLeft[index] = left;
      this.fodRight[index] = right;
      this.whispyDirection[index] = whispy;
      this.stadiumEvent[index] = stadiumEvent;
      this.stadiumType[index] = stadiumType;
      this.stadiumVisibleType[index] = stadiumVisibleType;
    }
  }
}
