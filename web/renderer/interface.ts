import type { CameraState } from './camera.js';

/** Phase-0 renderer boundary. Implementations own all graphics API state. */
export interface RenderSize { width: number; height: number; devicePixelRatio: number }
export interface SceneSnapshot { frame: number; slots: readonly object[]; camera: CameraState }
export interface Renderer {
  initialize(): void;
  resize(size: RenderSize): void;
  render(scene: SceneSnapshot): void;
  dispose(): void;
}

export function rendererNotImplemented(): never {
  throw new Error('WebGL2 renderer begins in phase 2');
}
