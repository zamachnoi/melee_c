export const REPLAY_HZ = 60;

export interface ClockSnapshot { frame: number; playing: boolean; ended: boolean }

/** Integer replay clock derived from an anchor, never accumulated frame deltas. */
export class ReplayClock {
  private frameValue: number;
  private playingValue = false;
  private anchorFrame: number;
  private anchorTime = 0;
  private readonly snapshotValue: ClockSnapshot;

  constructor(readonly startFrame: number, readonly endFrame: number, initialFrame = startFrame) {
    if (!Number.isInteger(startFrame) || !Number.isInteger(endFrame) || endFrame < startFrame) {
      throw new Error(`invalid replay clock range ${startFrame}..${endFrame}`);
    }
    this.frameValue = this.clamp(initialFrame);
    this.anchorFrame = this.frameValue;
    this.snapshotValue = { frame: this.frameValue, playing: false, ended: this.frameValue === endFrame };
  }

  get frame(): number { return this.frameValue; }
  get playing(): boolean { return this.playingValue; }

  play(now: number): ClockSnapshot {
    if (this.frameValue >= this.endFrame) this.frameValue = this.startFrame;
    this.anchorFrame = this.frameValue;
    this.anchorTime = now;
    this.playingValue = true;
    return this.snapshot();
  }

  pause(now: number): ClockSnapshot {
    this.sample(now);
    this.playingValue = false;
    return this.snapshot();
  }

  seek(frame: number, now: number): ClockSnapshot {
    this.frameValue = this.clamp(Math.round(frame));
    this.anchorFrame = this.frameValue;
    this.anchorTime = now;
    return this.snapshot();
  }

  step(delta: number, now: number): ClockSnapshot {
    this.playingValue = false;
    return this.seek(this.frameValue + Math.trunc(delta), now);
  }

  sample(now: number): ClockSnapshot {
    if (this.playingValue) {
      const elapsed = Math.max(0, now - this.anchorTime);
      this.frameValue = this.clamp(this.anchorFrame + Math.floor(elapsed * REPLAY_HZ / 1000));
      if (this.frameValue >= this.endFrame) this.playingValue = false;
    }
    return this.snapshot();
  }

  private clamp(frame: number): number { return Math.max(this.startFrame, Math.min(this.endFrame, frame)); }

  private snapshot(): ClockSnapshot {
    this.snapshotValue.frame = this.frameValue;
    this.snapshotValue.playing = this.playingValue;
    this.snapshotValue.ended = this.frameValue === this.endFrame;
    return this.snapshotValue;
  }
}
