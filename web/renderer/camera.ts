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

export interface CameraViewport { width: number; height: number }

export function panCamera(camera: CameraState, dx: number, dy: number): CameraState {
  return {
    ...camera,
    mode: 'free',
    centerX: camera.centerX - dx / camera.zoom,
    centerY: camera.centerY + dy / camera.zoom,
  };
}

export function screenToWorld(
  camera: CameraState, viewport: CameraViewport, x: number, y: number,
): { x: number; y: number } {
  return {
    x: camera.centerX + (x - viewport.width / 2) / camera.zoom,
    y: camera.centerY - (y - viewport.height / 2) / camera.zoom,
  };
}

/** Zoom while keeping the world point under the pointer stationary. */
export function zoomCameraAt(
  camera: CameraState, viewport: CameraViewport, x: number, y: number,
  factor: number, minZoom = 0.25, maxZoom = 40,
): CameraState {
  const anchor = screenToWorld(camera, viewport, x, y);
  const zoom = Math.min(maxZoom, Math.max(minZoom, camera.zoom * factor));
  return {
    ...camera,
    mode: 'free',
    zoom,
    centerX: anchor.x - (x - viewport.width / 2) / zoom,
    centerY: anchor.y + (y - viewport.height / 2) / zoom,
  };
}

export function setCamera(
  camera: CameraState, mode: CameraMode, centerX: number, centerY: number, zoom: number,
): CameraState {
  camera.mode = mode;
  camera.centerX = centerX;
  camera.centerY = centerY;
  camera.zoom = zoom;
  return camera;
}

export function fitCameraToBounds(
  camera: CameraState, viewport: CameraViewport,
  minX: number, minY: number, maxX: number, maxY: number,
  horizontalPadding = 50, verticalPadding = 70,
): CameraState {
  const width = Math.max(1, maxX - minX + horizontalPadding * 2);
  const height = Math.max(1, maxY - minY + verticalPadding * 2);
  return setCamera(camera, 'fit', (minX + maxX) / 2, (minY + maxY) / 2 + 8,
    Math.max(0.25, Math.min(40, Math.min(viewport.width / width, viewport.height / height))));
}

export function followCamera(
  camera: CameraState, viewport: CameraViewport, x: number, y: number, targetPort: number,
): CameraState {
  setCamera(camera, 'follow', x, y + 15, 5.5 * viewport.height / 720);
  camera.targetPort = targetPort;
  return camera;
}

/** Profile projection convention shared with render_pose_profile in C. */
export const PROFILE_AXES = Object.freeze({ horizontal: 'model-z', vertical: 'model-y', depth: 'model-x' });
