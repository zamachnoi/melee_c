export type CameraMode = 'free' | 'melee' | 'fit' | 'follow' | 'stage';

export interface CameraState {
  mode: CameraMode;
  centerX: number;
  centerY: number;
  zoom: number;
  distance: number;
  fov: number;
  verticalAngle: number;
  horizontalAngle: number;
  eyeX: number;
  eyeY: number;
  eyeZ: number;
  targetPort: number | null;
  smoothing: number;
}

export interface CameraViewport { width: number; height: number }

const RADIANS = Math.PI / 180;
const DEFAULT_FOV = 30;
const DEFAULT_DISTANCE = 356;
const DEFAULT_VERTICAL_ANGLE = -2;
const DEFAULT_HORIZONTAL_ANGLE = 0;
const DEFAULT_EYE_Y = 45;

export function createCameraState(): CameraState {
  const centerY = DEFAULT_EYE_Y + DEFAULT_DISTANCE * Math.tan(DEFAULT_VERTICAL_ANGLE * RADIANS);
  return {
    mode: 'melee',
    centerX: 0,
    centerY,
    zoom: zoomFromDistance({ width: 960, height: 720 }, DEFAULT_DISTANCE, DEFAULT_FOV),
    distance: DEFAULT_DISTANCE,
    fov: DEFAULT_FOV,
    verticalAngle: DEFAULT_VERTICAL_ANGLE,
    horizontalAngle: DEFAULT_HORIZONTAL_ANGLE,
    eyeX: 0,
    eyeY: DEFAULT_EYE_Y,
    eyeZ: DEFAULT_DISTANCE,
    targetPort: null,
    smoothing: 0,
  };
}

export function zoomFromDistance(viewport: CameraViewport, distance: number, fov = DEFAULT_FOV): number {
  return viewport.height / (2 * Math.max(1, distance) * Math.tan((fov * RADIANS) * 0.5));
}

export function distanceFromZoom(viewport: CameraViewport, zoom: number, fov = DEFAULT_FOV): number {
  return viewport.height / (2 * Math.max(0.25, zoom) * Math.tan((fov * RADIANS) * 0.5));
}

function syncEyeFromAngles(camera: CameraState, viewport?: CameraViewport): void {
  const fov = camera.fov > 0 ? camera.fov : DEFAULT_FOV;
  const distance = viewport
    ? distanceFromZoom(viewport, camera.zoom, fov)
    : (camera.distance > 0 ? camera.distance : DEFAULT_DISTANCE);
  camera.distance = distance;
  camera.eyeX = camera.centerX - distance * Math.tan(camera.horizontalAngle * RADIANS);
  camera.eyeY = camera.centerY - distance * Math.tan(camera.verticalAngle * RADIANS);
  camera.eyeZ = distance;
}

const LOOK_ANGLES = { verticalAngle: 0, horizontalAngle: 0 };
const VIEW_SCRATCH = new Float32Array(16);
const PROJ_SCRATCH = new Float32Array(16);
const AXIS_FORWARD = new Float32Array(3);
const AXIS_RIGHT = new Float32Array(3);
const AXIS_UP = new Float32Array(3);

function lookAnglesFromEye(
  interestX: number, interestY: number, eyeX: number, eyeY: number, eyeZ: number,
): { verticalAngle: number; horizontalAngle: number } {
  const depth = Math.max(1e-6, eyeZ);
  LOOK_ANGLES.verticalAngle = Math.atan((interestY - eyeY) / depth) / RADIANS;
  LOOK_ANGLES.horizontalAngle = Math.atan((interestX - eyeX) / depth) / RADIANS;
  return LOOK_ANGLES;
}

function writeNormalized(
  x: number, y: number, z: number, out: Float32Array | [number, number, number],
): void {
  const length = Math.hypot(x, y, z) || 1;
  out[0] = x / length;
  out[1] = y / length;
  out[2] = z / length;
}

function writeViewAxes(
  camera: CameraState,
  right: Float32Array | [number, number, number],
  up: Float32Array | [number, number, number],
  forward: Float32Array = AXIS_FORWARD,
): void {
  const distance = camera.distance > 0 ? camera.distance : DEFAULT_DISTANCE;
  const eyeX = Number.isFinite(camera.eyeZ) && camera.eyeZ > 0
    ? camera.eyeX
    : camera.centerX - distance * Math.tan(camera.horizontalAngle * RADIANS);
  const eyeY = Number.isFinite(camera.eyeZ) && camera.eyeZ > 0
    ? camera.eyeY
    : camera.centerY - distance * Math.tan(camera.verticalAngle * RADIANS);
  const eyeZ = Number.isFinite(camera.eyeZ) && camera.eyeZ > 0 ? camera.eyeZ : distance;
  writeNormalized(eyeX - camera.centerX, eyeY - camera.centerY, eyeZ, forward);
  writeNormalized(forward[2], 0, -forward[0], right);
  up[0] = forward[1] * right[2] - forward[2] * right[1];
  up[1] = forward[2] * right[0] - forward[0] * right[2];
  up[2] = forward[0] * right[1] - forward[1] * right[0];
}

export function cameraViewAxes(
  camera: CameraState,
  right: Float32Array | [number, number, number] = [0, 0, 0],
  up: Float32Array | [number, number, number] = [0, 0, 0],
): { right: Float32Array | [number, number, number]; up: Float32Array | [number, number, number] } {
  writeViewAxes(camera, right, up);
  return { right, up };
}

/**
 * HSD_CObjSetupViewingMtx: look from eye WObj at interest WObj with world up.
 * Camera_8002AF68 feeds those two points from CameraTransformState.
 */
export function cameraViewProjection(camera: CameraState, viewport: CameraViewport, out = new Float32Array(16)): Float32Array {
  const fov = camera.fov > 0 ? camera.fov : DEFAULT_FOV;
  const distance = camera.distance > 0 ? camera.distance : distanceFromZoom(viewport, camera.zoom, fov);
  const eyeX = Number.isFinite(camera.eyeZ) && camera.eyeZ > 0
    ? camera.eyeX
    : camera.centerX - distance * Math.tan(camera.horizontalAngle * RADIANS);
  const eyeY = Number.isFinite(camera.eyeZ) && camera.eyeZ > 0
    ? camera.eyeY
    : camera.centerY - distance * Math.tan(camera.verticalAngle * RADIANS);
  const eyeZ = Number.isFinite(camera.eyeZ) && camera.eyeZ > 0 ? camera.eyeZ : distance;
  writeViewAxes(camera, AXIS_RIGHT, AXIS_UP, AXIS_FORWARD);
  const xx = AXIS_RIGHT[0], xy = AXIS_RIGHT[1], xz = AXIS_RIGHT[2];
  const yx = AXIS_UP[0], yy = AXIS_UP[1], yz = AXIS_UP[2];
  const zx = AXIS_FORWARD[0], zy = AXIS_FORWARD[1], zz = AXIS_FORWARD[2];
  VIEW_SCRATCH[0] = xx; VIEW_SCRATCH[1] = yx; VIEW_SCRATCH[2] = zx; VIEW_SCRATCH[3] = 0;
  VIEW_SCRATCH[4] = xy; VIEW_SCRATCH[5] = yy; VIEW_SCRATCH[6] = zy; VIEW_SCRATCH[7] = 0;
  VIEW_SCRATCH[8] = xz; VIEW_SCRATCH[9] = yz; VIEW_SCRATCH[10] = zz; VIEW_SCRATCH[11] = 0;
  VIEW_SCRATCH[12] = -(xx * eyeX + xy * eyeY + xz * eyeZ);
  VIEW_SCRATCH[13] = -(yx * eyeX + yy * eyeY + yz * eyeZ);
  VIEW_SCRATCH[14] = -(zx * eyeX + zy * eyeY + zz * eyeZ);
  VIEW_SCRATCH[15] = 1;
  const aspect = viewport.width / Math.max(1, viewport.height);
  const near = Math.max(0.1, distance * 0.05);
  const far = Math.max(distance + 800, 131072);
  const f = 1 / Math.tan((fov * RADIANS) * 0.5);
  const nf = 1 / (near - far);
  PROJ_SCRATCH[0] = f / aspect; PROJ_SCRATCH[1] = 0; PROJ_SCRATCH[2] = 0; PROJ_SCRATCH[3] = 0;
  PROJ_SCRATCH[4] = 0; PROJ_SCRATCH[5] = f; PROJ_SCRATCH[6] = 0; PROJ_SCRATCH[7] = 0;
  PROJ_SCRATCH[8] = 0; PROJ_SCRATCH[9] = 0; PROJ_SCRATCH[10] = (far + near) * nf; PROJ_SCRATCH[11] = -1;
  PROJ_SCRATCH[12] = 0; PROJ_SCRATCH[13] = 0; PROJ_SCRATCH[14] = 2 * far * near * nf; PROJ_SCRATCH[15] = 0;
  for (let column = 0; column < 4; column++) {
    for (let row = 0; row < 4; row++) {
      let value = 0;
      for (let k = 0; k < 4; k++) value += PROJ_SCRATCH[k * 4 + row] * VIEW_SCRATCH[column * 4 + k];
      out[column * 4 + row] = value;
    }
  }
  return out;
}

export function panCamera(camera: CameraState, dx: number, dy: number): CameraState {
  const shiftX = -dx / camera.zoom;
  const shiftY = dy / camera.zoom;
  camera.mode = 'free';
  camera.centerX += shiftX;
  camera.centerY += shiftY;
  camera.eyeX += shiftX;
  camera.eyeY += shiftY;
  return camera;
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
  const distance = distanceFromZoom(viewport, zoom, camera.fov);
  const scale = distance / Math.max(1, camera.eyeZ || camera.distance);
  const centerX = anchor.x - (x - viewport.width / 2) / zoom;
  const centerY = anchor.y + (y - viewport.height / 2) / zoom;
  camera.mode = 'free';
  camera.zoom = zoom;
  camera.distance = distance;
  camera.eyeX = centerX + (camera.eyeX - camera.centerX) * scale;
  camera.eyeY = centerY + (camera.eyeY - camera.centerY) * scale;
  camera.centerX = centerX;
  camera.centerY = centerY;
  camera.eyeZ = distance;
  return camera;
}

export function setCamera(
  camera: CameraState, mode: CameraMode, centerX: number, centerY: number, zoom: number,
  viewport?: CameraViewport,
): CameraState {
  camera.mode = mode;
  camera.centerX = centerX;
  camera.centerY = centerY;
  camera.zoom = zoom;
  syncEyeFromAngles(camera, viewport);
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
    Math.max(0.25, Math.min(40, Math.min(viewport.width / width, viewport.height / height))), viewport);
}

export function followCamera(
  camera: CameraState, viewport: CameraViewport, x: number, y: number, targetPort: number,
): CameraState {
  setCamera(camera, 'follow', x, y + 15, 5.5 * viewport.height / 720, viewport);
  camera.targetPort = targetPort;
  return camera;
}

export interface StageCameraSource {
  cameraPosition: ArrayLike<number>;
  cameraFov: number;
  cameraVerticalAngle: number;
  cameraHorizontalAngle: number;
}

/** Authored Melee stage CObj: eye WObj plus aim angles, interest on z=0. */
export function meleeStageCamera(
  viewport: CameraViewport,
  stage?: StageCameraSource | null,
): Pick<CameraState, 'centerX' | 'centerY' | 'zoom' | 'distance' | 'fov' | 'verticalAngle' | 'horizontalAngle' | 'eyeX' | 'eyeY' | 'eyeZ'> {
  const position = stage?.cameraPosition;
  const eyeX = position?.[0] ?? 0;
  const eyeY = position?.[1] ?? DEFAULT_EYE_Y;
  const eyeZ = position && position[2] > 0 ? position[2] : DEFAULT_DISTANCE;
  const fov = stage?.cameraFov && stage.cameraFov > 0 ? stage.cameraFov : DEFAULT_FOV;
  const verticalAngle = stage ? stage.cameraVerticalAngle : DEFAULT_VERTICAL_ANGLE;
  const horizontalAngle = stage ? stage.cameraHorizontalAngle : DEFAULT_HORIZONTAL_ANGLE;
  const centerX = eyeX + eyeZ * Math.tan(horizontalAngle * RADIANS);
  const centerY = eyeY + eyeZ * Math.tan(verticalAngle * RADIANS);
  return {
    centerX, centerY, fov, distance: eyeZ, verticalAngle, horizontalAngle,
    eyeX, eyeY, eyeZ,
    zoom: zoomFromDistance(viewport, eyeZ, fov),
  };
}

export function applyMeleeCamera(camera: CameraState, viewport: CameraViewport, stage?: StageCameraSource | null): CameraState {
  Object.assign(camera, meleeStageCamera(viewport, stage));
  camera.mode = 'melee';
  camera.targetPort = null;
  return camera;
}

export const GAMEPLAY_FOV = 38;
export const GAMEPLAY_ASPECT = 1.2173333;

export interface CameraSubject { x: number; y: number; facing: number }

export interface GameplayCameraTarget {
  centerX: number;
  centerY: number;
  eyeX: number;
  eyeY: number;
  eyeZ: number;
  distance: number;
  fov: number;
  verticalAngle: number;
  horizontalAngle: number;
}

const SUBJECT_WEIGHT = [0, 1.5, 1.32, 1.16, 1];

/**
 * Camera_8002958C subject box + Camera_80029CF8 interest/eye solve.
 * HSD_CObj then looks from target_position at target_interest (Camera_8002AF68).
 */
export function gameplayCameraTarget(
  subjects: readonly CameraSubject[],
  out: GameplayCameraTarget = {
    centerX: 0, centerY: 0, eyeX: 0, eyeY: 0, eyeZ: 0,
    distance: 0, fov: 0, verticalAngle: 0, horizontalAngle: 0,
  },
): GameplayCameraTarget | null {
  if (!subjects.length) return null;
  const track = (SUBJECT_WEIGHT[Math.min(subjects.length, 4)] ?? 1) * 1.5;
  let minX = Infinity, maxX = -Infinity, minY = Infinity, maxY = -Infinity;
  for (const subject of subjects) {
    const x = Math.min(170, Math.max(-170, subject.x));
    const y = Math.min(120, Math.max(-60, subject.y + 10));
    const left = Math.max(-170, subject.facing >= 0 ? x - 9 * track : x - 22 * 1.5 * track);
    const right = Math.min(170, subject.facing >= 0 ? x + 22 * 1.5 * track : x + 9 * track);
    minX = Math.min(minX, left);
    maxX = Math.max(maxX, right);
    minY = Math.min(minY, Math.max(-60, y - 9 * track));
    maxY = Math.max(maxY, Math.min(120, y + 16 * track));
  }
  minY -= 10;

  const halfFov = (GAMEPLAY_FOV * 0.5) * RADIANS;
  const dx = maxX - minX, dy = maxY - minY;
  const spread = Math.max(dx, dy);
  const bias = spread <= 60 ? 0 : spread >= 120 ? 0.0682 : 0.0682 * (spread - 60) / 60;
  const base = (minY + maxY) * (0.5 - bias);
  const vangDeg = Math.min(5, Math.max(-7, -(base - 30) * 0.05)) - 10;
  const vang = vangDeg * RADIANS;
  const tanUp = Math.tan(halfFov + vang);
  const tanDown = Math.tan(halfFov - vang);
  const distY = dy / (tanUp + tanDown);
  const yOff = distY * Math.tan(vang);
  const interestY = yOff + (maxY - distY * tanUp);

  const xCenter = (minX + maxX) * 0.5;
  const hang = Math.min(17.5, Math.max(-17.5, -xCenter * 0.05)) * RADIANS;
  const tanRight = GAMEPLAY_ASPECT * Math.tan(halfFov - hang);
  const tanLeft = GAMEPLAY_ASPECT * Math.tan(halfFov + hang);
  const distX = dx / (tanRight + tanLeft);
  const xOff = GAMEPLAY_ASPECT * distX * Math.tan(hang);
  const interestX = (maxX - distX * tanRight) - xOff;
  const distance = Math.min(1000, Math.max(83, Math.max(distX, distY)));

  const eyeX = interestX + xOff;
  const eyeY = interestY - yOff;
  const eyeZ = distance;
  const angles = lookAnglesFromEye(interestX, interestY, eyeX, eyeY, eyeZ);
  out.centerX = interestX;
  out.centerY = interestY;
  out.eyeX = eyeX;
  out.eyeY = eyeY;
  out.eyeZ = eyeZ;
  out.distance = distance;
  out.fov = GAMEPLAY_FOV;
  out.verticalAngle = angles.verticalAngle;
  out.horizontalAngle = angles.horizontalAngle;
  return out;
}

export function applyGameplayCamera(
  camera: CameraState, viewport: CameraViewport, target: GameplayCameraTarget, mode: CameraMode,
): CameraState {
  return blendGameplayCamera(camera, viewport, target, mode, { interest: 1, eye: 1 });
}

/** Camera_80029AAC interest lerp vs Camera_80029C88 eye lerp. FD track-smooth 1.8 → ~9% / 27%. */
export const CAMERA_INTEREST_RATE = 0.09;
export const CAMERA_EYE_RATE = 0.27;

export function blendGameplayCamera(
  camera: CameraState, viewport: CameraViewport, target: GameplayCameraTarget, mode: CameraMode,
  rates: { interest: number; eye: number; steps?: number } = {
    interest: CAMERA_INTEREST_RATE, eye: CAMERA_EYE_RATE,
  },
): CameraState {
  const steps = Math.max(1, rates.steps ?? 1);
  const interestT = 1 - (1 - Math.min(1, Math.max(0, rates.interest))) ** steps;
  const eyeT = 1 - (1 - Math.min(1, Math.max(0, rates.eye))) ** steps;
  camera.centerX += (target.centerX - camera.centerX) * interestT;
  camera.centerY += (target.centerY - camera.centerY) * interestT;
  camera.eyeX += (target.eyeX - camera.eyeX) * eyeT;
  camera.eyeY += (target.eyeY - camera.eyeY) * eyeT;
  camera.eyeZ += (target.eyeZ - camera.eyeZ) * eyeT;
  camera.distance = camera.eyeZ;
  camera.fov = target.fov;
  const angles = lookAnglesFromEye(camera.centerX, camera.centerY, camera.eyeX, camera.eyeY, camera.eyeZ);
  camera.verticalAngle = angles.verticalAngle;
  camera.horizontalAngle = angles.horizontalAngle;
  camera.zoom = zoomFromDistance(viewport, Math.max(1, camera.distance), target.fov);
  camera.mode = mode;
  return camera;
}

export function gameplayLookTarget(
  interestX: number, interestY: number, distance: number, solved: GameplayCameraTarget,
  out: GameplayCameraTarget = {
    centerX: 0, centerY: 0, eyeX: 0, eyeY: 0, eyeZ: 0,
    distance: 0, fov: 0, verticalAngle: 0, horizontalAngle: 0,
  },
): GameplayCameraTarget {
  const eyeX = interestX + (solved.eyeX - solved.centerX);
  const eyeY = interestY + (solved.eyeY - solved.centerY);
  const eyeZ = distance;
  const angles = lookAnglesFromEye(interestX, interestY, eyeX, eyeY, eyeZ);
  out.centerX = interestX;
  out.centerY = interestY;
  out.eyeX = eyeX;
  out.eyeY = eyeY;
  out.eyeZ = eyeZ;
  out.distance = eyeZ;
  out.fov = solved.fov;
  out.verticalAngle = angles.verticalAngle;
  out.horizontalAngle = angles.horizontalAngle;
  return out;
}

/** Keep C's smoothed interest, but aim with Camera_80029CF8's eye offset. */
export function applySmoothedGameplayCamera(
  camera: CameraState, viewport: CameraViewport, interestX: number, interestY: number,
  distance: number, solved: GameplayCameraTarget, mode: CameraMode,
): CameraState {
  return applyGameplayCamera(camera, viewport, gameplayLookTarget(interestX, interestY, distance, solved), mode);
}

export function gameplayDistanceFromTimelineZoom(zoom: number): number {
  return 720 / (2 * Math.max(0.25, zoom) * Math.tan((GAMEPLAY_FOV * 0.5) * RADIANS));
}

/** Profile projection convention shared with render_pose_profile in C. */
export const PROFILE_AXES = Object.freeze({ horizontal: 'model-z', vertical: 'model-y', depth: 'model-x' });
