import { parseAnimations, type AnimationsAsset } from './assets/anims.js';
import { parseModel, type ModelAsset } from './assets/model.js';
import { parseStage, type StageAsset } from './assets/stage.js';
import { PoseEvaluator, evaluateStagePose, stageBoneYOffset, type PoseEvaluation } from './animation/pose.js';
import { ReplayClock } from './replay/clock.js';
import { ReplaySceneIndex } from './replay/scene.js';
import { parseTimeline, type Timeline } from './replay/timeline.js';
import {
  blendGameplayCamera, CAMERA_EYE_RATE, CAMERA_INTEREST_RATE,
  createCameraState, fitCameraToBounds, gameplayCameraTarget, gameplayDistanceFromTimelineZoom,
  gameplayLookTarget, meleeStageCamera,
  panCamera, zoomCameraAt,
  type CameraMode, type CameraState, type CameraSubject, type CameraViewport,
  type GameplayCameraTarget,
} from './renderer/camera.js';
import { isStageSection } from './renderer/static-pose.js';
import {
  EFFECT_ALIAS_KEYS, effectAssetUrl, parseEffectCatalog,
  type EffectCatalog, type EffectModelBank,
} from './renderer/effects.js';
import {
  WebGL2Renderer, type AnimatedFighter, type WebGLSceneSource,
} from './renderer/webgl2.js';

export { parseTimeline, measureTimeline } from './replay/timeline.js';
export { parseModel } from './assets/model.js';
export { parseAnimations } from './assets/anims.js';
export { parseStage } from './assets/stage.js';

interface ReplayListItem { id: string; name: string; file: string }
interface ManifestAsset { slot?: number; model?: string; animations?: string; stage?: string }
interface FighterAsset extends ManifestAsset { slot: number; model: string; animations: string }
interface ReplayManifest {
  id: string; name: string; startFrame: number; endFrame: number; stageId: number;
  stageName: string; timelineUrl: string; assets: ManifestAsset[];
}
interface CachedResult<T> { asset: T; wireBytes: number }
interface LoadedFighterAsset {
  manifest: FighterAsset;
  model: ModelAsset | null;
  animations: AnimationsAsset | null;
  wireBytes: number;
  warnings: string[];
}
interface FighterRuntime {
  slot: number;
  evaluator: PoseEvaluator | null;
  pose: PoseEvaluation | null;
  actionLabels: string[];
  animated: boolean;
  source: AnimatedFighter | null;
  warning: string | null;
}
interface HudEntry {
  root: HTMLDivElement;
  title: HTMLSpanElement;
  character: HTMLSpanElement;
  percent: HTMLSpanElement;
  stockPips: HTMLImageElement[];
  costume: number;
  lastPercent: number;
  lastStocks: number;
  lastCharacter: number;
  lastHeat: string;
  lastAbsent: boolean;
  lastUnavailable: boolean;
}
type AutomaticCameraMode = Extract<CameraMode, 'melee' | 'fit' | 'follow'>;

function element<T extends HTMLElement>(id: string): T {
  const value = document.getElementById(id);
  if (!value) throw new Error(`WebGL2 page is missing #${id}`);
  return value as T;
}

function errorMessage(error: unknown): string {
  return error instanceof Error ? error.message : String(error);
}

const CHARACTER_NAMES = [
  'Mario', 'Fox', 'Captain Falcon', 'Donkey Kong', 'Kirby', 'Bowser', 'Link', 'Sheik',
  'Ness', 'Peach', 'Popo', 'Nana', 'Pikachu', 'Samus', 'Yoshi', 'Jigglypuff',
  'Mewtwo', 'Luigi', 'Marth', 'Zelda', 'Young Link', 'Dr. Mario', 'Falco', 'Pichu',
  'Mr. Game & Watch', 'Ganondorf', 'Roy',
];

const CHARACTER_SLUGS = [
  'mario', 'fox', 'captain_falcon', 'donkey_kong', 'kirby', 'bowser', 'link', 'sheik',
  'ness', 'peach', 'popo', 'nana', 'pikachu', 'samus', 'yoshi', 'jigglypuff',
  'mewtwo', 'luigi', 'marth', 'zelda', 'young_link', 'dr_mario', 'falco', 'pichu',
  'mr_game_and_watch', 'ganondorf', 'roy',
];

function characterName(id: number): string {
  return CHARACTER_NAMES[id] ?? `Char ${id}`;
}

function stockIconUrl(characterId: number, costume: number): string {
  const slug = CHARACTER_SLUGS[characterId];
  return slug ? `/assets/v4/icons/${slug}-${costume}.png` : '';
}

function formatTimecode(frame: number): string {
  const sign = frame < 0 ? '-' : '';
  const totalSeconds = Math.floor(Math.abs(frame) / 60);
  return `${sign}${Math.floor(totalSeconds / 60)}:${String(totalSeconds % 60).padStart(2, '0')}`;
}

function percentHeat(percent: number): string {
  return percent >= 150 ? 'kill' : percent >= 100 ? 'high' : percent >= 50 ? 'mid' : '';
}

function actionLabels(animations: AnimationsAsset): string[] {
  return animations.actions.map(action => {
    const marker = action.name.indexOf('ACTION_');
    if (marker < 0) return action.name || 'unnamed';
    const start = marker + 7;
    const end = action.name.indexOf('_', start);
    return action.name.slice(start, end < 0 ? undefined : end);
  });
}

export async function bootWebGL2(): Promise<void> {
  const canvas = element<HTMLCanvasElement>('glCanvas');
  const viewportElement = element<HTMLDivElement>('viewport');
  const replaySelect = element<HTMLSelectElement>('replaySelect');
  const cameraModeSelect = element<HTMLSelectElement>('cameraMode');
  const frameSlider = element<HTMLInputElement>('frameSlider');
  const frameLabel = element<HTMLSpanElement>('frameLabel');
  const playButton = element<HTMLButtonElement>('playButton');
  const stepBack = element<HTMLButtonElement>('stepBack');
  const stepForward = element<HTMLButtonElement>('stepForward');
  const status = element<HTMLDivElement>('status');
  const diagnostics = element<HTMLDivElement>('diagnostics');
  const sceneLabel = element<HTMLDivElement>('sceneLabel');
  const resetCamera = element<HTMLButtonElement>('resetCamera');
  const hud = element<HTMLDivElement>('hud');
  const warningOverlay = element<HTMLDivElement>('warningOverlay');
  const debugOverlay = element<HTMLPreElement>('debugOverlay');
  const debugToggle = element<HTMLButtonElement>('debugToggle');

  const fillReplaySelect = (replays: ReplayListItem[], selectedId?: string): void => {
    replaySelect.replaceChildren(...replays.map(replay => {
      const option = document.createElement('option');
      option.value = replay.id; option.textContent = replay.name; return option;
    }));
    if (!replays.length) {
      const empty = document.createElement('option');
      empty.disabled = true;
      empty.textContent = 'No replays';
      replaySelect.append(empty);
    }
    replaySelect.dataset.filled = '1';
    if (selectedId && replays.some(replay => replay.id === selectedId)) replaySelect.value = selectedId;
  };

  status.textContent = 'Loading replay list…';
  const replayResponse = await fetch('/api/replays');
  if (!replayResponse.ok) throw new Error(`/api/replays: HTTP ${replayResponse.status}`);
  const replays = await replayResponse.json() as ReplayListItem[];
  const requestedReplay = new URLSearchParams(location.search).get('replay');
  const selectedReplay = replays.some(replay => replay.id === requestedReplay) ? requestedReplay! : replays[0]?.id;
  fillReplaySelect(replays, selectedReplay);

  status.textContent = 'Checking WebGL2 capabilities…';
  const renderer = new WebGL2Renderer(canvas, message => { status.textContent = message; });
  renderer.initialize();
  status.textContent = 'Capability gate passed; loading replay…';

  const modelCache = new Map<string, ModelAsset>();
  const animationCache = new Map<string, AnimationsAsset>();
  const stageCache = new Map<string, StageAsset>();
  const effectModelCache = new Map<string, ModelAsset>();
  const modelPending = new Map<string, Promise<CachedResult<ModelAsset>>>();
  const animationPending = new Map<string, Promise<CachedResult<AnimationsAsset>>>();
  const stagePending = new Map<string, Promise<CachedResult<StageAsset>>>();
  const effectModelPending = new Map<string, Promise<CachedResult<ModelAsset>>>();
  let effectCatalog: EffectCatalog | null | undefined;

  let timeline: Timeline | null = null;
  let sceneIndex: ReplaySceneIndex | null = null;
  let manifest: ReplayManifest | null = null;
  let runtimes: FighterRuntime[] = [];
  let hudEntries: HudEntry[] = [];
  let clock: ReplayClock | null = null;
  let source: WebGLSceneSource = {
    stageSections: [], stageScale: 1, fighters: [], items: [], itemStart: 0, itemEnd: 0,
    stageState: { fodLeft: Number.NaN, fodRight: Number.NaN, whispyDirection: -1, stadiumEvent: -1, stadiumType: -1 },
  };
  let frame = 0;
  let camera: CameraState = createCameraState();
  const viewportSize: CameraViewport = { width: 960, height: 720 };
  let preferredCameraMode: AutomaticCameraMode = 'melee';
  let followSlot = 0;
  let lastCameraIndex: number | null = null;
  let lastBlendMode: AutomaticCameraMode | null = null;
  let stageCameraSource: StageAsset | null = null;
  let requestCount = 0;
  let cameraMoves = 0;
  let loadController: AbortController | null = null;
  let loadedWireBytes = 0;
  let animationSamples = 0, animationTotalMs = 0, animationMaxMs = 0;
  let frameSamples = 0, frameTotalMs = 0, frameMaxMs = 0;
  let presentedFrames = 0, droppedPresentations = 0, consecutivePresented = 0, longestConsecutive = 0;
  let animationFrameId = 0, lastDiagnosticsAt = 0, lastChromeAt = 0;
  let currentItemCount = 0;
  let assetWarnings: string[] = [];
  let debugVisible = false;
  const injectedAssetFailure = new URLSearchParams(location.search).get('failAsset');
  const cameraSubjectList: CameraSubject[] = [];
  const cameraSubjectPool: CameraSubject[] = Array.from({ length: 8 }, () => ({ x: 0, y: 0, facing: 1 }));
  const solvedCamera: GameplayCameraTarget = {
    centerX: 0, centerY: 0, eyeX: 0, eyeY: 0, eyeZ: 0,
    distance: 0, fov: 0, verticalAngle: 0, horizontalAngle: 0,
  };
  const lookCamera: GameplayCameraTarget = {
    centerX: 0, centerY: 0, eyeX: 0, eyeY: 0, eyeZ: 0,
    distance: 0, fov: 0, verticalAngle: 0, horizontalAngle: 0,
  };
  const fodOffsets = new Map<number, number>();

  const trackedFetch = async (url: string, init?: RequestInit): Promise<Response> => {
    requestCount++;
    if (injectedAssetFailure && url.includes(injectedAssetFailure))
      throw new Error(`${url}: injected asset failure`);
    const response = await fetch(url, init);
    if (!response.ok) throw new Error(`${url}: HTTP ${response.status}`);
    return response;
  };

  const loadCached = async <T>(
    url: string, cache: Map<string, T>, pending: Map<string, Promise<CachedResult<T>>>,
    parse: (buffer: ArrayBuffer) => T,
  ): Promise<CachedResult<T>> => {
    const cached = cache.get(url);
    if (cached) return { asset: cached, wireBytes: 0 };
    const inFlight = pending.get(url);
    if (inFlight) return { asset: (await inFlight).asset, wireBytes: 0 };
    const promise = (async (): Promise<CachedResult<T>> => {
      const response = await trackedFetch(url);
      const buffer = await response.arrayBuffer();
      const asset = parse(buffer);
      cache.set(url, asset);
      return { asset, wireBytes: buffer.byteLength };
    })();
    pending.set(url, promise);
    try { return await promise; }
    finally { pending.delete(url); }
  };

  const loadFighterAsset = async (asset: FighterAsset): Promise<LoadedFighterAsset> => {
    let model: ModelAsset | null = null, animations: AnimationsAsset | null = null, wireBytes = 0;
    const warnings: string[] = [];
    try {
      const result = await loadCached(asset.model, modelCache, modelPending, parseModel);
      model = result.asset; wireBytes += result.wireBytes;
    } catch (error) { warnings.push(`slot ${asset.slot} model unavailable: ${errorMessage(error)}`); }
    try {
      const result = await loadCached(asset.animations, animationCache, animationPending, parseAnimations);
      animations = result.asset; wireBytes += result.wireBytes;
    } catch (error) { warnings.push(`slot ${asset.slot} animations unavailable: ${errorMessage(error)}`); }
    return { manifest: asset, model, animations, wireBytes, warnings };
  };

  const loadEffectCatalog = async (): Promise<EffectCatalog | null> => {
    if (effectCatalog !== undefined) return effectCatalog;
    try {
      const response = await trackedFetch('/assets/v4/effects.json');
      effectCatalog = parseEffectCatalog(await response.json());
      return effectCatalog;
    } catch (error) {
      effectCatalog = null;
      assetWarnings.push(`effect catalog unavailable: ${errorMessage(error)}`);
      return null;
    }
  };

  const loadEffectBank = async (timeline: Timeline): Promise<{ bank: EffectModelBank; wireBytes: number }> => {
    const byAlias: Record<string, ModelAsset> = {};
    const byItem: Record<number, ModelAsset> = {};
    const catalog = await loadEffectCatalog();
    if (!catalog) return { bank: { byAlias, byItem }, wireBytes: 0 };
    const files = new Map<string, { aliases: string[]; items: number[] }>();
    const add = (file: string | undefined, alias?: string, item?: number): void => {
      if (!file) return;
      let entry = files.get(file);
      if (!entry) {
        entry = { aliases: [], items: [] };
        files.set(file, entry);
      }
      if (alias) entry.aliases.push(alias);
      if (item !== undefined) entry.items.push(item);
    };
    for (const alias of EFFECT_ALIAS_KEYS) add(catalog.aliases[alias], alias);
    const itemIds = new Set<number>([0x36, 0x37, 0x38, 0x39]);
    for (const item of timeline.items) itemIds.add(item.typeId);
    for (const id of itemIds) add(catalog.items[String(id)], undefined, id);
    let wireBytes = 0;
    await Promise.all([...files.entries()].map(async ([file, refs]) => {
      try {
        const result = await loadCached(effectAssetUrl(file), effectModelCache, effectModelPending, parseModel);
        wireBytes += result.wireBytes;
        for (const alias of refs.aliases) byAlias[alias] = result.asset;
        for (const id of refs.items) byItem[id] = result.asset;
      } catch (error) {
        assetWarnings.push(`${file} unavailable: ${errorMessage(error)}`);
      }
    }));
    return { bank: { byAlias, byItem }, wireBytes };
  };

  const measureViewport = (): void => {
    const rect = canvas.getBoundingClientRect();
    viewportSize.width = Math.max(1, rect.width);
    viewportSize.height = Math.max(1, rect.height);
  };

  const cameraSubjects = (index: number, slotIndex?: number): CameraSubject[] => {
    cameraSubjectList.length = 0;
    if (!timeline) return cameraSubjectList;
    for (let i = 0; i < timeline.slots.length; i++) {
      if (slotIndex !== undefined && i !== slotIndex) continue;
      const slot = timeline.slots[i];
      if (!slot?.active || !slot.presence[index] || !slot.stocks[index]) continue;
      if (slotIndex === undefined && slot.follower) continue;
      const x = slot.x[index], y = slot.y[index];
      if (Math.abs(x) > 300 || y < -120 || y > 240) continue;
      const subject = cameraSubjectPool[cameraSubjectList.length] ?? { x: 0, y: 0, facing: 1 };
      subject.x = x;
      subject.y = y;
      subject.facing = slot.facing[index];
      cameraSubjectList.push(subject);
    }
    return cameraSubjectList;
  };

  const blendRates = { interest: 1, eye: 1, steps: 1 };
  const SNAP_BLEND = { interest: 1, eye: 1, steps: 1 };
  const cameraBlendRates = (index: number, interestAlreadySmoothed: boolean): { interest: number; eye: number; steps: number } => {
    const previous = lastCameraIndex;
    const jumped = previous === null || lastBlendMode !== preferredCameraMode
      || Math.abs(index - previous) > 4;
    const steps = jumped ? 1 : Math.max(1, Math.abs(index - previous) || 1);
    lastCameraIndex = index;
    lastBlendMode = preferredCameraMode;
    if (jumped) {
      blendRates.interest = 1;
      blendRates.eye = 1;
      blendRates.steps = 1;
      return blendRates;
    }
    blendRates.interest = interestAlreadySmoothed ? 1 : CAMERA_INTEREST_RATE;
    blendRates.eye = CAMERA_EYE_RATE;
    blendRates.steps = steps;
    return blendRates;
  };

  const applyOriginalCamera = (index: number, rates?: { interest: number; eye: number; steps: number }): void => {
    const solved = gameplayCameraTarget(cameraSubjects(index), solvedCamera);
    const blend = rates ?? cameraBlendRates(index, true);
    if (timeline?.camera && solved) {
      blendGameplayCamera(
        camera, viewportSize,
        gameplayLookTarget(
          timeline.camera.x[index], timeline.camera.y[index],
          gameplayDistanceFromTimelineZoom(timeline.camera.zoom[index]),
          solved, lookCamera,
        ),
        'melee', blend,
      );
    } else if (solved) {
      blendGameplayCamera(camera, viewportSize, solved, 'melee', blend);
    } else {
      Object.assign(camera, meleeStageCamera(viewportSize, stageCameraSource));
    }
    camera.mode = 'melee';
    camera.targetPort = null;
  };

  const applyFitCamera = (index: number): void => {
    if (!timeline) return;
    let minX = Infinity, minY = Infinity, maxX = -Infinity, maxY = -Infinity;
    for (let slotIndex = 0; slotIndex < timeline.slots.length; slotIndex++) {
      const slot = timeline.slots[slotIndex];
      if (!slot.active || !slot.presence[index] || !slot.stocks[index]) continue;
      const x = slot.x[index], y = slot.y[index];
      if (Math.abs(x) > 300 || y < -120 || y > 240) continue;
      minX = Math.min(minX, x); maxX = Math.max(maxX, x);
      minY = Math.min(minY, y); maxY = Math.max(maxY, y + 25);
    }
    if (Number.isFinite(minX)) fitCameraToBounds(camera, viewportSize, minX, minY, maxX, maxY);
    else applyOriginalCamera(index);
  };

  const applyFollowCamera = (index: number): void => {
    if (!timeline) return;
    const slot = timeline.slots[followSlot];
    const solved = gameplayCameraTarget(cameraSubjects(index, followSlot), solvedCamera);
    if (solved && slot?.presence[index]) {
      blendGameplayCamera(camera, viewportSize, solved, 'follow', cameraBlendRates(index, false));
      camera.targetPort = slot.port;
    } else applyOriginalCamera(index, SNAP_BLEND);
  };

  const applyAutomaticCamera = (index: number): void => {
    if (camera.mode === 'free') return;
    if (preferredCameraMode === 'fit') applyFitCamera(index);
    else if (preferredCameraMode === 'follow') applyFollowCamera(index);
    else applyOriginalCamera(index);
  };

  const updateDiagnostics = (): void => {
    const caps = renderer.capabilities;
    const animationAverage = animationSamples ? animationTotalMs / animationSamples : 0;
    const frameAverage = frameSamples ? frameTotalMs / frameSamples : 0;
    diagnostics.textContent = [
      `${requestCount} load requests`, `0 frame-image requests`, `${cameraMoves} camera moves`,
      `${presentedFrames} presented`, `${droppedPresentations} dropped`, `${longestConsecutive} consecutive`,
      `${source.fighters.length} fighters`, `${currentItemCount} items`, `${assetWarnings.length} warnings`,
      `pose ${animationAverage.toFixed(2)}ms avg/${animationMaxMs.toFixed(2)}ms max`,
      `work ${frameAverage.toFixed(2)}ms avg/${frameMaxMs.toFixed(2)}ms max`,
      `${(loadedWireBytes / 1048576).toFixed(2)} MiB wire`, `${(renderer.gpuBytes / 1048576).toFixed(2)} MiB GPU`,
      `max texture ${caps.maxTextureSize}${renderer.overlayTruncated ? ' · overlay truncated' : ''}`,
    ].join(' · ');
  };

  const renderCurrent = (): void => { renderer.draw(camera); };

  /* Fountain of Dreams: side platforms are section 2 bones 2/3 plus a second
     material pass on section 3 bones 1/2.  Replay heights are world Y; the
     mesh is DAT-local and drawn at stageScale 0.75.  Until the first 0x3F
     event, use the in-game start heights (left 16.125, right 22.125). */
  const FOD_LEFT_START = 16.125;
  const FOD_RIGHT_START = 22.125;
  const FOD_PLATFORM_SECTIONS = [
    { index: 2, leftBone: 2, rightBone: 3 },
    { index: 3, leftBone: 1, rightBone: 2 },
  ] as const;
  type FodStageEntry = {
    model: ModelAsset; boneRows: Float32Array; poseVersion: number;
    leftBone: number; rightBone: number;
  };
  let fodStageEntries: FodStageEntry[] = [];
  let lastFodLeft = Number.NaN;
  let lastFodRight = Number.NaN;

  const fodWorldHeight = (value: number, fallback: number): number =>
    Number.isFinite(value) ? value : fallback;

  const setupFodPlatforms = (
    sceneSource: WebGLSceneSource, sections: readonly ModelAsset[],
  ): void => {
    fodStageEntries = [];
    lastFodLeft = Number.NaN;
    lastFodRight = Number.NaN;
    if (!timeline || timeline.stageId !== 2) return;
    const animated: FodStageEntry[] = [];
    for (const spec of FOD_PLATFORM_SECTIONS) {
      const section = sections[spec.index];
      if (!section?.vertexCount) continue;
      animated.push({
        model: section, boneRows: new Float32Array(0), poseVersion: 0,
        leftBone: spec.leftBone, rightBone: spec.rightBone,
      });
    }
    if (!animated.length) return;
    const skip = new Set(animated.map(entry => entry.model));
    sceneSource.stageSections = sceneSource.stageSections.filter(
      candidate => !skip.has(candidate));
    fodStageEntries = animated;
    sceneSource.stageAnimated = fodStageEntries;
    updateFodPlatforms(sceneSource, Number.NaN, Number.NaN);
  };

  const updateFodPlatforms = (
    sceneSource: WebGLSceneSource, fodLeft: number, fodRight: number,
  ): void => {
    if (!fodStageEntries.length) return;
    const left = fodWorldHeight(fodLeft, FOD_LEFT_START);
    const right = fodWorldHeight(fodRight, FOD_RIGHT_START);
    sceneSource.stageState.fodLeft = left;
    sceneSource.stageState.fodRight = right;
    const changed = left !== lastFodLeft || right !== lastFodRight;
    lastFodLeft = left;
    lastFodRight = right;
    if (!changed && fodStageEntries.every(entry => entry.boneRows.length)) return;
    const leftOffset = stageBoneYOffset(left, sceneSource.stageScale);
    const rightOffset = stageBoneYOffset(right, sceneSource.stageScale);
    for (const entry of fodStageEntries) {
      fodOffsets.clear();
      fodOffsets.set(entry.leftBone, leftOffset);
      fodOffsets.set(entry.rightBone, rightOffset);
      entry.boneRows = evaluateStagePose(entry.model, fodOffsets, entry.boneRows.length
        ? entry.boneRows : undefined);
      entry.poseVersion += 1;
    }
  };

  const updateDebug = (index: number): void => {
    if (!debugVisible || !timeline) return;
    let text = `frame ${frame} | items ${currentItemCount} | stage FoD ${source.stageState.fodLeft}/${source.stageState.fodRight}`;
    text += ` | whispy ${source.stageState.whispyDirection} | stadium ${source.stageState.stadiumEvent}:${source.stageState.stadiumType}`;
    for (const runtime of runtimes) {
      const slot = timeline.slots[runtime.slot];
      const pose = runtime.pose;
      text += `\ns${runtime.slot} P${slot.port + 1}${slot.follower ? ' Nana' : ''}`;
      if (!runtime.source) text += ` unavailable: ${runtime.warning ?? 'model missing'}`;
      else if (!slot.presence[index]) text += ' absent';
      else text += ` action ${slot.animationIndex[index]}→${pose?.resolvedAction ?? 'bind'} af ${slot.animationFrame[index].toFixed(2)}`
        + ` pos ${slot.x[index].toFixed(2)},${slot.y[index].toFixed(2)} facing ${slot.facing[index]}`;
    }
    if (assetWarnings.length) text += `\nwarnings: ${assetWarnings.join(' | ')}`;
    debugOverlay.textContent = text;
  };

  const updateFrameLabel = (): void => {
    if (!timeline) return;
    const text = `${formatTimecode(frame)} / ${formatTimecode(timeline.endFrame)}`;
    if (frameLabel.textContent !== text) frameLabel.textContent = text;
    if (frameSlider.valueAsNumber !== frame) frameSlider.value = String(frame);
    const span = timeline.endFrame - timeline.startFrame;
    const progress = span <= 0 ? 0 : ((frame - timeline.startFrame) / span) * 100;
    frameSlider.style.setProperty('--progress', `${progress}%`);
    frameSlider.setAttribute('aria-valuetext', `frame ${frame}`);
  };

  const updateFrame = (nextFrame: number, updateLabel = true): void => {
    if (!timeline || !sceneIndex) return;
    const workStarted = performance.now();
    frame = Math.max(timeline.startFrame, Math.min(timeline.endFrame, Math.round(nextFrame)));
    const index = frame - timeline.startFrame;
    const animationStarted = performance.now();
    for (let runtimeIndex = 0; runtimeIndex < runtimes.length; runtimeIndex++) {
      const runtime = runtimes[runtimeIndex];
      const slot = timeline.slots[runtime.slot];
      const fighter = runtime.source;
      if (fighter) {
        fighter.visible = Boolean(slot.presence[index]);
        fighter.rootX = slot.x[index]; fighter.rootY = slot.y[index]; fighter.facing = slot.facing[index];
        fighter.actionState = slot.actionState[index]; fighter.shield = slot.shield[index];
        fighter.percent = slot.percent[index]; fighter.stocks = slot.stocks[index];
        fighter.characterId = slot.character[index];
        if (fighter.visible && runtime.evaluator && runtime.animated) {
          runtime.pose = runtime.evaluator.evaluate(slot.animationIndex[index], slot.animationFrame[index]);
          fighter.poseVersion++;
          fighter.actionName = runtime.pose.actionName;
        } else {
          fighter.actionName = runtime.pose?.actionName ?? null;
        }
      }
      const hudEntry = hudEntries[runtimeIndex];
      if (hudEntry) {
        const absent = !slot.presence[index];
        const unavailable = !fighter;
        if (hudEntry.lastAbsent !== absent) {
          hudEntry.root.classList.toggle('absent', absent);
          hudEntry.lastAbsent = absent;
        }
        if (hudEntry.lastUnavailable !== unavailable) {
          hudEntry.root.classList.toggle('unavailable', unavailable);
          hudEntry.lastUnavailable = unavailable;
        }
        const characterId = slot.character[index];
        if (hudEntry.lastCharacter !== characterId) {
          hudEntry.character.textContent = unavailable ? 'unavailable' : characterName(characterId);
          const icon = unavailable ? '' : stockIconUrl(characterId, hudEntry.costume);
          for (const pip of hudEntry.stockPips) {
            if (icon) pip.src = icon;
          }
          hudEntry.lastCharacter = characterId;
        }
        const percent = Math.floor(slot.percent[index]);
        if (hudEntry.lastPercent !== percent) {
          hudEntry.percent.textContent = unavailable ? '—' : `${percent}%`;
          hudEntry.lastPercent = percent;
        }
        const heat = unavailable ? '' : percentHeat(percent);
        if (hudEntry.lastHeat !== heat) {
          hudEntry.percent.className = heat ? `hud-percent ${heat}` : 'hud-percent';
          hudEntry.lastHeat = heat;
        }
        const stocks = slot.stocks[index];
        if (hudEntry.lastStocks !== stocks) {
          for (let pip = 0; pip < hudEntry.stockPips.length; pip++) {
            hudEntry.stockPips[pip].classList.toggle('lost', pip >= stocks);
          }
          hudEntry.lastStocks = stocks;
        }
      }
    }
    const animationElapsed = performance.now() - animationStarted;
    animationSamples++;
    animationTotalMs += animationElapsed;
    animationMaxMs = Math.max(animationMaxMs, animationElapsed);
    source.itemStart = sceneIndex.itemStarts[index];
    source.itemEnd = sceneIndex.itemEnds[index];
    currentItemCount = source.itemEnd - source.itemStart;
    source.stageState.fodLeft = sceneIndex.fodLeft[index];
    source.stageState.fodRight = sceneIndex.fodRight[index];
    source.stageState.whispyDirection = sceneIndex.whispyDirection[index];
    source.stageState.stadiumEvent = sceneIndex.stadiumEvent[index];
    source.stageState.stadiumType = sceneIndex.stadiumType[index];
    updateFodPlatforms(source, sceneIndex.fodLeft[index], sceneIndex.fodRight[index]);
    applyAutomaticCamera(index);
    renderCurrent();
    const workElapsed = performance.now() - workStarted;
    frameSamples++;
    frameTotalMs += workElapsed;
    frameMaxMs = Math.max(frameMaxMs, workElapsed);
    if (updateLabel) updateFrameLabel();
    updateDebug(index);
  };

  const setPlayState = (playing: boolean): void => {
    playButton.classList.toggle('playing', playing);
    playButton.setAttribute('aria-pressed', String(playing));
    playButton.setAttribute('aria-label', playing ? 'Pause' : 'Play');
  };

  const playbackTick = (now: number): void => {
    if (!clock) return;
    const snapshot = clock.sample(now);
    if (snapshot.frame !== frame) {
      const delta = snapshot.frame - frame;
      if (delta === 1) {
        consecutivePresented++;
        longestConsecutive = Math.max(longestConsecutive, consecutivePresented);
      } else {
        if (delta > 1) droppedPresentations += delta - 1;
        consecutivePresented = 0;
      }
      presentedFrames++;
      updateFrame(snapshot.frame, false);
    }
    if (now - lastChromeAt >= 100) { updateFrameLabel(); lastChromeAt = now; }
    if (now - lastDiagnosticsAt >= 250) { updateDiagnostics(); lastDiagnosticsAt = now; }
    if (snapshot.playing) animationFrameId = requestAnimationFrame(playbackTick);
    else { animationFrameId = 0; setPlayState(false); updateFrameLabel(); updateDiagnostics(); }
  };

  const stopPlayback = (now = performance.now()): void => {
    if (clock?.playing) clock.pause(now);
    if (animationFrameId) cancelAnimationFrame(animationFrameId);
    animationFrameId = 0;
    setPlayState(false);
  };

  const togglePlayback = (): void => {
    if (!clock) return;
    const now = performance.now();
    if (clock.playing) {
      const snapshot = clock.pause(now);
      if (snapshot.frame !== frame) updateFrame(snapshot.frame);
      else updateFrameLabel();
      if (animationFrameId) cancelAnimationFrame(animationFrameId);
      animationFrameId = 0;
      setPlayState(false);
    } else {
      clock.seek(frame, now); clock.play(now); consecutivePresented = 0;
      setPlayState(true); animationFrameId = requestAnimationFrame(playbackTick);
    }
    updateDiagnostics();
  };

  const seek = (nextFrame: number): void => {
    if (!clock) return;
    clock.seek(nextFrame, performance.now()); consecutivePresented = 0;
    updateFrame(clock.frame); updateDiagnostics();
  };

  const step = (amount: number): void => {
    if (!clock) return;
    stopPlayback();
    const snapshot = clock.step(amount, performance.now()); consecutivePresented = 0;
    updateFrame(snapshot.frame); updateDiagnostics();
  };

  const resize = (): void => {
    measureViewport();
    renderer.resize({ width: viewportSize.width, height: viewportSize.height, devicePixelRatio: window.devicePixelRatio || 1 });
    if (timeline && camera.mode !== 'free') applyAutomaticCamera(frame - timeline.startFrame);
    renderCurrent();
  };
  const resizeObserver = new ResizeObserver(resize);
  resizeObserver.observe(viewportElement);
  addEventListener('resize', resize);
  addEventListener('pagehide', () => {
    stopPlayback(); loadController?.abort(); resizeObserver.disconnect();
    removeEventListener('resize', resize); renderer.dispose();
  }, { once: true });
  resize();

  let dragging = false;
  let lastX = 0, lastY = 0;
  canvas.addEventListener('pointerdown', event => {
    dragging = true; lastX = event.clientX; lastY = event.clientY; canvas.setPointerCapture(event.pointerId);
  });
  canvas.addEventListener('pointermove', event => {
    if (!dragging) return;
    camera = panCamera(camera, event.clientX - lastX, event.clientY - lastY);
    lastX = event.clientX; lastY = event.clientY; cameraMoves++; renderCurrent();
  });
  const stopDragging = (): void => { dragging = false; };
  canvas.addEventListener('pointerup', stopDragging);
  canvas.addEventListener('pointercancel', stopDragging);
  canvas.addEventListener('wheel', event => {
    event.preventDefault();
    const rect = canvas.getBoundingClientRect();
    camera = zoomCameraAt(camera, viewportSize, event.clientX - rect.left, event.clientY - rect.top,
      Math.exp(-event.deltaY * 0.001));
    cameraMoves++; renderCurrent();
  }, { passive: false });

  const reset = (): void => {
    if (!timeline) return;
    lastCameraIndex = null;
    lastBlendMode = null;
    camera.mode = preferredCameraMode; applyAutomaticCamera(frame - timeline.startFrame);
    cameraMoves++; renderCurrent(); updateDiagnostics();
  };
  canvas.addEventListener('dblclick', reset);
  resetCamera.addEventListener('click', reset);
  const highlightFollow = (): void => {
    for (let i = 0; i < hudEntries.length; i++) {
      const following = preferredCameraMode === 'follow' && runtimes[i]?.slot === followSlot;
      hudEntries[i].root.classList.toggle('following', following);
    }
  };

  const followFighter = (slotIndex: number): void => {
    followSlot = slotIndex;
    preferredCameraMode = 'follow';
    cameraModeSelect.value = 'follow';
    camera.mode = 'follow';
    reset();
    highlightFollow();
  };

  cameraModeSelect.addEventListener('change', () => {
    preferredCameraMode = cameraModeSelect.value as AutomaticCameraMode;
    camera.mode = preferredCameraMode; reset(); highlightFollow();
  });
  frameSlider.addEventListener('input', () => seek(Number(frameSlider.value)));
  playButton.addEventListener('click', togglePlayback);
  stepBack.addEventListener('click', () => step(-1));
  stepForward.addEventListener('click', () => step(1));
  debugToggle.addEventListener('click', () => {
    debugVisible = !debugVisible;
    debugOverlay.hidden = !debugVisible;
    debugToggle.setAttribute('aria-pressed', String(debugVisible));
    if (timeline) updateDebug(frame - timeline.startFrame);
  });

  addEventListener('keydown', event => {
    const target = event.target as HTMLElement | null;
    if (target?.matches('select,input,button')) return;
    if (event.code === 'Space') { event.preventDefault(); togglePlayback(); }
    else if (event.code === 'ArrowLeft') { event.preventDefault(); step(-1); }
    else if (event.code === 'ArrowRight') { event.preventDefault(); step(1); }
    else if (event.code === 'Home' && timeline) { event.preventDefault(); seek(timeline.startFrame); }
    else if (event.code === 'End' && timeline) { event.preventDefault(); seek(timeline.endFrame); }
    else if (event.code === 'Backquote') { event.preventDefault(); debugToggle.click(); }
    else if (/^Digit[1-4]$/.test(event.code) && timeline) {
      const port = Number(event.code.at(-1)) - 1;
      const slotIndex = timeline.slots.findIndex(slot => slot.active && !slot.follower && slot.port === port);
      if (slotIndex >= 0) followFighter(slotIndex);
    }
  });

  const buildHud = (): void => {
    hudEntries = runtimes.map(runtime => {
      const slot = timeline!.slots[runtime.slot];
      const player = timeline!.players.find(value => value.port === slot.port);
      const root = document.createElement('div');
      root.className = slot.follower ? 'hud-player follower' : 'hud-player';
      root.dataset.slot = String(runtime.slot);
      root.dataset.port = String(slot.port);
      root.title = 'Follow this fighter';
      const meta = document.createElement('div');
      meta.className = 'hud-meta';
      const title = document.createElement('span');
      title.className = 'hud-name';
      title.textContent = `${player?.name || `P${slot.port + 1}`}${slot.follower ? ' · Nana' : ''}`;
      const character = document.createElement('span');
      character.className = 'hud-char';
      const percent = document.createElement('span');
      percent.className = 'hud-percent';
      percent.textContent = '0%';
      const stocks = document.createElement('div');
      stocks.className = 'hud-stocks';
      const pipCount = Math.max(player?.startingStocks ?? 4, 1);
      const costume = player?.costume ?? 0;
      const stockPips = Array.from({ length: pipCount }, () => {
        const pip = document.createElement('img');
        pip.className = 'hud-stock';
        pip.alt = '';
        pip.width = 24;
        pip.height = 24;
        pip.decoding = 'async';
        pip.addEventListener('error', () => pip.classList.add('missing'));
        pip.addEventListener('load', () => pip.classList.remove('missing'));
        return pip;
      });
      stocks.append(...stockPips);
      meta.append(title, character, stocks);
      root.append(meta, percent);
      root.addEventListener('click', () => followFighter(runtime.slot));
      return {
        root, title, character, percent, stockPips, costume,
        lastPercent: Number.NaN, lastStocks: -1, lastCharacter: -1, lastHeat: '',
        lastAbsent: false, lastUnavailable: false,
      };
    });
    hud.replaceChildren(...hudEntries.map(entry => entry.root));
    highlightFollow();
  };

  const loadReplay = async (id: string): Promise<void> => {
    stopPlayback(); loadController?.abort();
    const controller = new AbortController(); loadController = controller;
    loadedWireBytes = 0; assetWarnings = [];
    status.classList.remove('error'); status.textContent = 'Loading manifest…';
    try {
      const manifestResponse = await trackedFetch(`/api/replays/${id}/manifest`, { signal: controller.signal });
      manifest = await manifestResponse.json() as ReplayManifest;
      const fighterAssets = manifest.assets.filter((value): value is FighterAsset =>
        typeof value.model === 'string' && typeof value.animations === 'string' && value.slot !== undefined)
        .sort((a, b) => a.slot - b.slot);
      const stageAsset = manifest.assets.find(value => typeof value.stage === 'string');
      status.textContent = 'Loading complete replay scene and reusable assets…';
      const timelinePromise = (async () => {
        const response = await trackedFetch(manifest!.timelineUrl, { signal: controller.signal });
        const buffer = await response.arrayBuffer();
        return { timeline: parseTimeline(buffer), wireBytes: buffer.byteLength };
      })();
      const fightersPromise = Promise.all(fighterAssets.map(loadFighterAsset));
      const stagePromise: Promise<CachedResult<StageAsset> | null> = stageAsset?.stage
        ? loadCached(stageAsset.stage, stageCache, stagePending, parseStage).catch(error => {
          assetWarnings.push(`stage unavailable: ${errorMessage(error)}`); return null;
        })
        : Promise.resolve(null);
      const effectsPromise = timelinePromise.then(({ timeline }) => loadEffectBank(timeline));
      const [timelineResult, loadedFighters, loadedStage, effectResult] = await Promise.all(
        [timelinePromise, fightersPromise, stagePromise, effectsPromise]);
      if (controller.signal.aborted) return;
      timeline = timelineResult.timeline;
      sceneIndex = new ReplaySceneIndex(timeline);
      loadedWireBytes = timelineResult.wireBytes + (loadedStage?.wireBytes ?? 0) + effectResult.wireBytes;
      for (const loaded of loadedFighters) loadedWireBytes += loaded.wireBytes;
      for (const loaded of loadedFighters) assetWarnings.push(...loaded.warnings);
      if (!stageAsset?.stage) assetWarnings.push(`stage asset missing for ${manifest.stageName}`);

      runtimes = loadedFighters.map(loaded => {
        const slot = timeline!.slots[loaded.manifest.slot];
        const player = timeline!.players.find(value => value.port === slot.port);
        if (!loaded.model) return {
          slot: loaded.manifest.slot, evaluator: null, pose: null, actionLabels: [], animated: false,
          source: null, warning: loaded.warnings.join('; ') || 'model unavailable',
        };
        const animations: AnimationsAsset = loaded.animations ?? { schema: 4, actions: [], keyCount: 0 };
        const evaluator = new PoseEvaluator(loaded.model, animations);
        const pose = evaluator.evaluateBindPose();
        const fighter: AnimatedFighter = {
          slot: loaded.manifest.slot, port: slot.port, follower: slot.follower,
          model: loaded.model, boneRows: pose.boneRows, poseVersion: 0,
          label: `${player?.name || `P${slot.port + 1}`}${slot.follower ? ' · Nana' : ''}`,
          rootX: 0, rootY: 0, facing: 1, visible: true, actionState: 0, actionName: null,
          characterId: slot.character[0] ?? 0, shield: 0, percent: 0,
          stocks: player?.startingStocks ?? 0,
        };
        return {
          slot: loaded.manifest.slot, evaluator, pose,
          actionLabels: loaded.animations ? actionLabels(loaded.animations) : [],
          animated: Boolean(loaded.animations), source: fighter,
          warning: loaded.animations ? null : loaded.warnings.join('; ') || 'animations unavailable; using bind pose',
        };
      });
      const fighters = runtimes.flatMap(runtime => runtime.source ? [runtime.source] : []);
      let stageSections: readonly ModelAsset[] = [];
      let stageScale = 1;
      if (loadedStage) {
        stageScale = loadedStage.asset.scale;
        stageCameraSource = loadedStage.asset;
        stageSections = loadedStage.asset.sections.filter(isStageSection);
        if (!stageSections.length) assetWarnings.push(`no visible stage sections for ${manifest.stageName}`);
      } else {
        stageCameraSource = null;
      }
      source = {
        stageSections, stageScale, fighters, items: timeline.items, itemStart: 0, itemEnd: 0,
        stageState: { fodLeft: Number.NaN, fodRight: Number.NaN, whispyDirection: -1, stadiumEvent: -1, stadiumType: -1 },
        effectModels: effectResult.bank,
      };
      setupFodPlatforms(source, loadedStage?.asset.sections ?? []);
      renderer.setScene(source);
      buildHud();
      const slotParameter = new URLSearchParams(location.search).get('slot');
      const requestedSlot = slotParameter === null ? Number.NaN : Number(slotParameter);
      const selected = runtimes.some(runtime => runtime.slot === requestedSlot)
        ? requestedSlot : (runtimes.find(runtime => !timeline!.slots[runtime.slot].follower)?.slot ?? runtimes[0]?.slot ?? 0);
      followSlot = selected;
      highlightFollow();
      frameSlider.min = String(timeline.startFrame); frameSlider.max = String(timeline.endFrame);
      const frameParameter = new URLSearchParams(location.search).get('frame');
      const requestedFrame = frameParameter === null ? Number.NaN : Number(frameParameter);
      const initialFrame = Number.isFinite(requestedFrame) && requestedFrame >= timeline.startFrame && requestedFrame <= timeline.endFrame
        ? requestedFrame : Math.max(timeline.startFrame, Math.min(timeline.endFrame, 8));
      clock = new ReplayClock(timeline.startFrame, timeline.endFrame, initialFrame);
      camera.mode = preferredCameraMode;
      lastCameraIndex = null;
      lastBlendMode = null;
      sceneLabel.textContent = manifest.stageName;
      warningOverlay.textContent = assetWarnings.join('\n'); warningOverlay.hidden = assetWarnings.length === 0;
      document.title = `Melee WebGL2 — ${manifest.name}`;
      updateFrame(initialFrame);
      status.textContent = assetWarnings.length
        ? `Replay ready with ${assetWarnings.length} visible asset warning${assetWarnings.length === 1 ? '' : 's'}`
        : 'Complete WebGL2 replay ready — all scene state and cameras are local';
      const url = new URL(location.href);
      if (url.searchParams.get('renderer') === 'software') url.searchParams.delete('renderer');
      url.searchParams.set('replay', id);
      url.searchParams.set('slot', String(selected)); history.replaceState(null, '', url);
      updateDiagnostics();
      if (new URLSearchParams(location.search).get('autoplay') === '1') togglePlayback();
    } catch (error) {
      if (controller.signal.aborted) return;
      status.classList.add('error'); status.textContent = errorMessage(error); throw error;
    }
  };

  const replayFile = element<HTMLInputElement>('replayFile');
  replayFile.addEventListener('change', () => {
    const file = replayFile.files?.[0];
    replayFile.value = '';
    if (!file) return;
    void (async () => {
      status.classList.remove('error');
      status.textContent = `Uploading ${file.name}…`;
      try {
        const response = await trackedFetch('/api/replays', {
          method: 'POST', body: file, headers: { 'X-Replay-Name': file.name },
        });
        const created = await response.json() as { id: string };
        const uploaded = await trackedFetch('/api/replays').then(value => value.json()) as ReplayListItem[];
        fillReplaySelect(uploaded, created.id);
        await loadReplay(created.id);
      } catch (error) {
        status.classList.add('error'); status.textContent = errorMessage(error);
      }
    })();
  });
  replaySelect.addEventListener('change', () => {
    const url = new URL(location.href); url.searchParams.delete('slot'); history.replaceState(null, '', url);
    void loadReplay(replaySelect.value).catch(console.error);
  });

  if (!selectedReplay) {
    sceneLabel.textContent = 'No replay loaded';
    status.textContent = 'No replay is available. Load an .slp file to start.';
    return;
  }
  await loadReplay(selectedReplay);
}

if (typeof document !== 'undefined' && document.documentElement.dataset.renderer === 'webgl2') {
  void bootWebGL2().catch(error => {
    const status = document.getElementById('status');
    if (status) { status.classList.add('error'); status.textContent = errorMessage(error); }
    console.error(error);
  });
}
