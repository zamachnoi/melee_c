export type CameraMode = 'free' | 'melee' | 'fit' | 'follow' | 'stage';

export interface CameraState {
  mode: CameraMode;
  centerX: number;
  centerY: number;
  zoom: number;
  targetPort: number | null;
  smoothing: number;
}

export function createCameraState(): CameraState {
  return { mode: 'melee', centerX: 0, centerY: 0, zoom: 1, targetPort: null, smoothing: 0 };
}

/** Profile projection convention shared with render_pose_profile in C. */
export const PROFILE_AXES = Object.freeze({ horizontal: 'model-z', vertical: 'model-y', depth: 'model-x' });
