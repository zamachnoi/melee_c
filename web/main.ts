import { parseAnimations, type AnimationsAsset } from './assets/anims.js';
import { parseModel, type ModelAsset } from './assets/model.js';
import { parseStage, type StageAsset } from './assets/stage.js';
import { PoseEvaluator, type PoseEvaluation } from './animation/pose.js';
import { ReplayClock } from './replay/clock.js';
import { ReplaySceneIndex } from './replay/scene.js';
import { parseTimeline, type Timeline } from './replay/timeline.js';
import {
  fitCameraToBounds, followCamera, panCamera, setCamera, zoomCameraAt,
  type CameraMode, type CameraState, type CameraViewport,
} from './renderer/camera.js';
import { isAcceptedFdSection } from './renderer/static-pose.js';
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
interface HudEntry { root: HTMLDivElement; title: HTMLSpanElement; state: HTMLSpanElement }
type AutomaticCameraMode = Extract<CameraMode, 'melee' | 'fit' | 'follow'>;

function element<T extends HTMLElement>(id: string): T {
  const value = document.getElementById(id);
  if (!value) throw new Error(`WebGL2 page is missing #${id}`);
  return value as T;
}

function errorMessage(error: unknown): string {
  return error instanceof Error ? error.message : String(error);
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
  const fighterSelect = element<HTMLSelectElement>('fighterSelect');
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

  status.textContent = 'Checking WebGL2 capabilities…';
  const renderer = new WebGL2Renderer(canvas, message => { status.textContent = message; });
  renderer.initialize();
  status.textContent = 'Capability gate passed; loading replay list…';

  const modelCache = new Map<string, ModelAsset>();
  const animationCache = new Map<string, AnimationsAsset>();
  const stageCache = new Map<string, StageAsset>();
  const modelPending = new Map<string, Promise<CachedResult<ModelAsset>>>();
  const animationPending = new Map<string, Promise<CachedResult<AnimationsAsset>>>();
  const stagePending = new Map<string, Promise<CachedResult<StageAsset>>>();

  let timeline: Timeline | null = null;
  let sceneIndex: ReplaySceneIndex | null = null;
  let manifest: ReplayManifest | null = null;
  let runtimes: FighterRuntime[] = [];
  let runtimeBySlot: Array<FighterRuntime | null> = Array(8).fill(null) as Array<FighterRuntime | null>;
  let hudEntries: HudEntry[] = [];
  let clock: ReplayClock | null = null;
  let source: WebGLSceneSource = {
    stageSections: [], stageScale: 1, fighters: [], items: [], itemStart: 0, itemEnd: 0,
    stageState: { fodLeft: Number.NaN, fodRight: Number.NaN, whispyDirection: -1, stadiumEvent: -1, stadiumType: -1 },
  };
  let frame = 0;
  let camera: CameraState = { mode: 'melee', centerX: 0, centerY: 32, zoom: 3.45, targetPort: null, smoothing: 0 };
  const viewportSize: CameraViewport = { width: 960, height: 720 };
  let preferredCameraMode: AutomaticCameraMode = 'melee';
  let followSlot = 0;
  let requestCount = 0;
  let cameraMoves = 0;
  let loadController: AbortController | null = null;
  let loadedWireBytes = 0;
  let animationSamples = 0, animationTotalMs = 0, animationMaxMs = 0;
  let frameSamples = 0, frameTotalMs = 0, frameMaxMs = 0;
  let presentedFrames = 0, droppedPresentations = 0, consecutivePresented = 0, longestConsecutive = 0;
  let animationFrameId = 0, lastDiagnosticsAt = 0;
  let currentItemCount = 0;
  let assetWarnings: string[] = [];
  let debugVisible = false;
  const injectedAssetFailure = new URLSearchParams(location.search).get('failAsset');

  const trackedFetch = async (url: string, signal?: AbortSignal): Promise<Response> => {
    requestCount++;
    if (injectedAssetFailure && url.includes(injectedAssetFailure))
      throw new Error(`${url}: injected asset failure`);
    const response = await fetch(url, { signal });
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

  const measureViewport = (): void => {
    const rect = canvas.getBoundingClientRect();
    viewportSize.width = Math.max(1, rect.width);
    viewportSize.height = Math.max(1, rect.height);
  };

  const selectedSlot = (): number => Number(fighterSelect.value);

  const applyOriginalCamera = (index: number): void => {
    if (!timeline?.camera) setCamera(camera, 'melee', 0, 32, 3.45 * viewportSize.height / 720);
    else setCamera(camera, 'melee', timeline.camera.x[index], timeline.camera.y[index],
      timeline.camera.zoom[index] * viewportSize.height / 720);
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
    if (slot?.presence[index]) followCamera(camera, viewportSize, slot.x[index], slot.y[index], slot.port);
    else applyOriginalCamera(index);
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
        if (fighter.visible && runtime.evaluator && runtime.animated) {
          runtime.pose = runtime.evaluator.evaluate(slot.animationIndex[index], slot.animationFrame[index]);
          fighter.poseVersion++;
        }
      }
      const hudEntry = hudEntries[runtimeIndex];
      if (hudEntry) {
        hudEntry.root.classList.toggle('absent', !slot.presence[index]);
        hudEntry.root.classList.toggle('unavailable', !fighter);
        hudEntry.state.textContent = fighter
          ? `${Math.round(slot.percent[index])}% · ×${slot.stocks[index]}`
          : 'asset unavailable';
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
    applyAutomaticCamera(index);
    renderCurrent();
    const workElapsed = performance.now() - workStarted;
    frameSamples++;
    frameTotalMs += workElapsed;
    frameMaxMs = Math.max(frameMaxMs, workElapsed);
    frameSlider.value = String(frame);
    if (updateLabel) {
      const selected = runtimeBySlot[selectedSlot()];
      const pose = selected?.pose;
      const resolvedAction = pose?.resolvedAction ?? null;
      const action = !selected?.source ? 'asset unavailable'
        : resolvedAction === null ? (selected.animated ? 'bind' : 'bind-only')
          : (selected.actionLabels[resolvedAction] ?? 'unknown');
      const mapping = pose?.fallback ? ` · ${pose.requestedAction}→${resolvedAction ?? 'bind'}` : '';
      frameLabel.textContent = `frame ${frame} · ${action}${mapping}`;
    }
    updateDebug(index);
  };

  const setPlayState = (playing: boolean): void => {
    playButton.textContent = playing ? 'Pause' : 'Play';
    playButton.setAttribute('aria-pressed', String(playing));
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
      updateFrame(snapshot.frame);
    }
    if (now - lastDiagnosticsAt >= 250) { updateDiagnostics(); lastDiagnosticsAt = now; }
    if (snapshot.playing) animationFrameId = requestAnimationFrame(playbackTick);
    else { animationFrameId = 0; setPlayState(false); updateDiagnostics(); }
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
    lastX = event.clientX; lastY = event.clientY; cameraMoves++; renderCurrent(); updateDiagnostics();
  });
  const stopDragging = (): void => { dragging = false; };
  canvas.addEventListener('pointerup', stopDragging);
  canvas.addEventListener('pointercancel', stopDragging);
  canvas.addEventListener('wheel', event => {
    event.preventDefault();
    const rect = canvas.getBoundingClientRect();
    camera = zoomCameraAt(camera, viewportSize, event.clientX - rect.left, event.clientY - rect.top,
      Math.exp(-event.deltaY * 0.001));
    cameraMoves++; renderCurrent(); updateDiagnostics();
  }, { passive: false });

  const reset = (): void => {
    if (!timeline) return;
    camera.mode = preferredCameraMode; applyAutomaticCamera(frame - timeline.startFrame);
    cameraMoves++; renderCurrent(); updateDiagnostics();
  };
  canvas.addEventListener('dblclick', reset);
  resetCamera.addEventListener('click', reset);
  cameraModeSelect.addEventListener('change', () => {
    preferredCameraMode = cameraModeSelect.value as AutomaticCameraMode;
    camera.mode = preferredCameraMode; reset();
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
      if (slotIndex >= 0) {
        followSlot = slotIndex; fighterSelect.value = String(slotIndex);
        preferredCameraMode = 'follow'; cameraModeSelect.value = 'follow'; camera.mode = 'follow'; reset();
      }
    }
  });

  const buildHud = (): void => {
    hudEntries = runtimes.map(runtime => {
      const root = document.createElement('div');
      const title = document.createElement('span');
      const state = document.createElement('span');
      root.className = 'hud-player'; root.dataset.slot = String(runtime.slot);
      title.className = 'hud-name'; state.className = 'hud-state';
      const slot = timeline!.slots[runtime.slot];
      const player = timeline!.players.find(value => value.port === slot.port);
      title.textContent = `${player?.name || `P${slot.port + 1}`}${slot.follower ? ' · Nana' : ''}`;
      root.append(title, state);
      return { root, title, state };
    });
    hud.replaceChildren(...hudEntries.map(entry => entry.root));
  };

  const loadReplay = async (id: string): Promise<void> => {
    stopPlayback(); loadController?.abort();
    const controller = new AbortController(); loadController = controller;
    loadedWireBytes = 0; assetWarnings = [];
    status.classList.remove('error'); status.textContent = 'Loading manifest…';
    try {
      const manifestResponse = await trackedFetch(`/api/replays/${id}/manifest`, controller.signal);
      manifest = await manifestResponse.json() as ReplayManifest;
      const fighterAssets = manifest.assets.filter((value): value is FighterAsset =>
        typeof value.model === 'string' && typeof value.animations === 'string' && value.slot !== undefined)
        .sort((a, b) => a.slot - b.slot);
      const stageAsset = manifest.assets.find(value => typeof value.stage === 'string');
      status.textContent = 'Loading complete replay scene and reusable assets…';
      const timelinePromise = (async () => {
        const response = await trackedFetch(manifest!.timelineUrl, controller.signal);
        const buffer = await response.arrayBuffer();
        return { timeline: parseTimeline(buffer), wireBytes: buffer.byteLength };
      })();
      const fightersPromise = Promise.all(fighterAssets.map(loadFighterAsset));
      const stagePromise: Promise<CachedResult<StageAsset> | null> = stageAsset?.stage
        ? loadCached(stageAsset.stage, stageCache, stagePending, parseStage).catch(error => {
          assetWarnings.push(`stage unavailable: ${errorMessage(error)}`); return null;
        })
        : Promise.resolve(null);
      const [timelineResult, loadedFighters, loadedStage] = await Promise.all([timelinePromise, fightersPromise, stagePromise]);
      if (controller.signal.aborted) return;
      timeline = timelineResult.timeline;
      sceneIndex = new ReplaySceneIndex(timeline);
      loadedWireBytes = timelineResult.wireBytes + (loadedStage?.wireBytes ?? 0);
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
          rootX: 0, rootY: 0, facing: 1, visible: true, actionState: 0, shield: 0, percent: 0,
          stocks: player?.startingStocks ?? 0,
        };
        return {
          slot: loaded.manifest.slot, evaluator, pose,
          actionLabels: loaded.animations ? actionLabels(loaded.animations) : [],
          animated: Boolean(loaded.animations), source: fighter,
          warning: loaded.animations ? null : loaded.warnings.join('; ') || 'animations unavailable; using bind pose',
        };
      });
      runtimeBySlot = Array(8).fill(null) as Array<FighterRuntime | null>;
      for (const runtime of runtimes) runtimeBySlot[runtime.slot] = runtime;
      const fighters = runtimes.flatMap(runtime => runtime.source ? [runtime.source] : []);
      let stageSections: readonly ModelAsset[] = [];
      let stageScale = 1;
      if (loadedStage) {
        stageScale = loadedStage.asset.scale;
        stageSections = manifest.stageName === 'Final Destination'
          ? loadedStage.asset.sections.filter(isAcceptedFdSection)
          : loadedStage.asset.sections;
        if (!stageSections.length) assetWarnings.push(`no visible stage sections for ${manifest.stageName}`);
      }
      source = {
        stageSections, stageScale, fighters, items: timeline.items, itemStart: 0, itemEnd: 0,
        stageState: { fodLeft: Number.NaN, fodRight: Number.NaN, whispyDirection: -1, stadiumEvent: -1, stadiumType: -1 },
      };
      renderer.setScene(source);
      buildHud();
      fighterSelect.replaceChildren(...runtimes.map(runtime => {
        const option = document.createElement('option');
        option.value = String(runtime.slot);
        option.textContent = runtime.source?.label ?? `slot ${runtime.slot} · unavailable`;
        return option;
      }));
      const slotParameter = new URLSearchParams(location.search).get('slot');
      const requestedSlot = slotParameter === null ? Number.NaN : Number(slotParameter);
      const selected = runtimes.some(runtime => runtime.slot === requestedSlot)
        ? requestedSlot : (runtimes.find(runtime => !timeline!.slots[runtime.slot].follower)?.slot ?? runtimes[0]?.slot ?? 0);
      fighterSelect.value = String(selected); followSlot = selected;
      frameSlider.min = String(timeline.startFrame); frameSlider.max = String(timeline.endFrame);
      const frameParameter = new URLSearchParams(location.search).get('frame');
      const requestedFrame = frameParameter === null ? Number.NaN : Number(frameParameter);
      const initialFrame = Number.isFinite(requestedFrame) && requestedFrame >= timeline.startFrame && requestedFrame <= timeline.endFrame
        ? requestedFrame : Math.max(timeline.startFrame, Math.min(timeline.endFrame, 8));
      clock = new ReplayClock(timeline.startFrame, timeline.endFrame, initialFrame);
      camera.mode = preferredCameraMode;
      sceneLabel.textContent = `${manifest.name} · ${fighters.length}/${fighterAssets.length} fighters · ${manifest.stageName} · complete local scene`;
      warningOverlay.textContent = assetWarnings.join('\n'); warningOverlay.hidden = assetWarnings.length === 0;
      document.title = `Melee WebGL2 — ${manifest.name}`;
      updateFrame(initialFrame);
      status.textContent = assetWarnings.length
        ? `Replay ready with ${assetWarnings.length} visible asset warning${assetWarnings.length === 1 ? '' : 's'}`
        : 'Complete WebGL2 replay ready — all scene state and cameras are local';
      const url = new URL(location.href);
      url.searchParams.set('renderer', 'webgl2'); url.searchParams.set('replay', id);
      url.searchParams.set('slot', String(selected)); history.replaceState(null, '', url);
      updateDiagnostics();
      if (new URLSearchParams(location.search).get('autoplay') === '1') togglePlayback();
    } catch (error) {
      if (controller.signal.aborted) return;
      status.classList.add('error'); status.textContent = errorMessage(error); throw error;
    }
  };

  const replayResponse = await trackedFetch('/api/replays');
  const replays = await replayResponse.json() as ReplayListItem[];
  replaySelect.replaceChildren(...replays.map(replay => {
    const option = document.createElement('option'); option.value = replay.id; option.textContent = replay.name; return option;
  }));
  if (!replays.length) throw new Error('No replay is available. Upload one in the software viewer first.');
  const requestedReplay = new URLSearchParams(location.search).get('replay');
  replaySelect.value = replays.some(replay => replay.id === requestedReplay) ? requestedReplay! : replays[0].id;
  replaySelect.addEventListener('change', () => {
    const url = new URL(location.href); url.searchParams.delete('slot'); history.replaceState(null, '', url);
    void loadReplay(replaySelect.value).catch(console.error);
  });
  fighterSelect.addEventListener('change', () => {
    followSlot = selectedSlot();
    const url = new URL(location.href); url.searchParams.set('slot', fighterSelect.value); history.replaceState(null, '', url);
    if (preferredCameraMode === 'follow') reset();
    if (timeline) updateFrame(frame);
  });
  await loadReplay(replaySelect.value);
}

if (typeof document !== 'undefined' && document.documentElement.dataset.renderer === 'webgl2') {
  void bootWebGL2().catch(error => {
    const status = document.getElementById('status');
    if (status) { status.classList.add('error'); status.textContent = errorMessage(error); }
    console.error(error);
  });
}
