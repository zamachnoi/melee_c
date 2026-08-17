import type { ModelAsset } from '../assets/model.js';
import type { TimelineItem } from '../replay/timeline.js';
import type { CameraState } from './camera.js';
import { cameraViewAxes, cameraViewProjection } from './camera.js';
import {
  createHexPrism, createUnitCylinder, createUvSphere, EffectKind,
  FALCO_LASER_COLOR, FIRE_COLOR, FIRE_RADIUS, fitEffectScale, FOX_LASER_COLOR,
  isFalcoLaser, isFalcoPhantasm, isFirefoxAction, isFoxIllusion, isFoxLaser,
  isShieldState, isShineAction, isSpacie, laserLength,
  LASER_THICKNESS, portShieldColor, shieldRadius, SHIELD_Y_OFFSET,
  SHINE_COLOR, SHINE_RADIUS, xyExtent, xyRadius, type EffectModelBank, type GeneratedMesh,
} from './effects.js';
import type { Renderer, RenderSize, SceneSnapshot } from './interface.js';
import { positionBounds, transformBindPose } from './static-pose.js';

const MESH_VERTEX_SHADER = `#version 300 es
layout(location=0) in vec3 a_position;
layout(location=1) in vec2 a_uv;
layout(location=2) in vec4 a_color;
layout(location=3) in vec4 a_weights;
layout(location=4) in uvec4 a_bones;

uniform mat4 u_viewProjection;
uniform vec2 u_replayRoot;
uniform float u_facing;
uniform float u_modelScale;
uniform bool u_profile;
uniform bool u_skinned;
uniform bool u_billboard;
uniform bool u_ray;
uniform bool u_viewBillboard;
uniform vec3 u_cameraRight;
uniform vec3 u_cameraUp;
uniform vec2 u_dir;
uniform highp sampler2D u_boneMatrices;

out vec2 v_uv;
out vec4 v_color;

vec3 applyBone(uint bone, int matrixOffset, vec3 position) {
  int y = int(bone);
  vec4 point = vec4(position, 1.0);
  vec4 row0 = texelFetch(u_boneMatrices, ivec2(matrixOffset, y), 0);
  vec4 row1 = texelFetch(u_boneMatrices, ivec2(matrixOffset + 1, y), 0);
  vec4 row2 = texelFetch(u_boneMatrices, ivec2(matrixOffset + 2, y), 0);
  return vec3(dot(row0, point), dot(row1, point), dot(row2, point));
}

vec3 posedPosition() {
  if (!u_skinned) return a_position;
  int influences = 0;
  for (int i = 0; i < 4; i++) if (a_weights[i] > 0.0001) influences++;
  int matrixOffset = influences > 1 ? 3 : 0;
  vec3 accumulated = vec3(0.0);
  float totalWeight = 0.0;
  for (int i = 0; i < 4; i++) {
    float weight = a_weights[i];
    if (weight <= 0.0001) continue;
    accumulated += applyBone(a_bones[i], matrixOffset, a_position) * weight;
    totalWeight += weight;
  }
  return totalWeight > 0.0 ? accumulated / totalWeight : a_position;
}

void main() {
  vec3 position = posedPosition();
  vec3 world;
  if (u_profile) {
    world = vec3(position.z * u_facing + u_replayRoot.x, position.y + u_replayRoot.y, position.x);
  } else if (u_viewBillboard) {
    vec3 scaled = position * u_modelScale;
    world = vec3(u_replayRoot.x, u_replayRoot.y, 0.0)
      + u_cameraRight * scaled.x
      + u_cameraUp * scaled.y;
  } else if (u_billboard) {
    vec2 dir = dot(u_dir, u_dir) > 0.0001 ? normalize(u_dir) : vec2(1.0, 0.0);
    vec2 perp = vec2(-dir.y, dir.x);
    vec3 scaled = position * u_modelScale;
    // Item rays (lasers) live in fighter space: Z is length, Y is up, X is depth.
    vec3 local = u_ray ? vec3(scaled.z, scaled.y, scaled.x) : scaled;
    world = vec3(u_replayRoot.x, u_replayRoot.y, 0.0)
      + vec3(dir * local.x + perp * local.y, local.z);
  } else {
    world = vec3(position.x, position.y, position.z) * u_modelScale;
  }
  gl_Position = u_viewProjection * vec4(world, 1.0);
  v_uv = a_uv;
  v_color = a_color;
}`;

const MESH_FRAGMENT_SHADER = `#version 300 es
precision mediump float;
uniform sampler2D u_texture;
uniform sampler2D u_detail;
uniform vec4 u_tint;
uniform bool u_mirrorMask;
in vec2 v_uv;
in vec4 v_color;
out vec4 outColor;

float mirrorCoord(float t) {
  t = abs(t);
  float cycle = floor(t);
  float frac = t - cycle;
  return mod(cycle, 2.0) < 0.5 ? frac : 1.0 - frac;
}

void main() {
  vec4 tex = texture(u_texture, v_uv);
  if (u_mirrorMask) {
    vec2 maskUv = vec2(mirrorCoord(v_uv.x * 2.0), mirrorCoord(v_uv.y * 2.0 - 1.0));
    tex = texture(u_texture, maskUv);
    vec4 detail = texture(u_detail, v_uv);
    tex = vec4(detail.rgb * tex.rgb, detail.a * tex.a);
  }
  outColor = tex * u_tint * v_color;
  if (outColor.a <= 0.0) discard;
}`;

const BACKDROP_VERTEX_SHADER = `#version 300 es
out vec2 v_uv;
void main() {
  vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
  v_uv = p;
  gl_Position = vec4(p * 2.0 - 1.0, 1.0, 1.0);
}`;

const BACKDROP_FRAGMENT_SHADER = `#version 300 es
precision mediump float;
in vec2 v_uv;
out vec4 outColor;
void main() {
  vec3 top = vec3(5.0, 7.0, 18.0) / 255.0;
  vec3 bottom = vec3(24.0, 19.0, 46.0) / 255.0;
  outColor = vec4(mix(bottom, top, v_uv.y), 1.0);
}`;

const STAR_VERTEX_SHADER = `#version 300 es
layout(location=0) in vec2 a_position;
layout(location=1) in vec3 a_color;
uniform float u_pointSize;
out vec3 v_color;
void main() {
  gl_Position = vec4(a_position, 0.0, 1.0);
  gl_PointSize = u_pointSize;
  v_color = a_color;
}`;

const STAR_FRAGMENT_SHADER = `#version 300 es
precision mediump float;
in vec3 v_color;
out vec4 outColor;
void main() { outColor = vec4(v_color, 1.0); }
`;

const OVERLAY_VERTEX_SHADER = `#version 300 es
layout(location=0) in vec2 a_position;
layout(location=1) in vec4 a_color;
layout(location=2) in float a_size;
uniform mat4 u_viewProjection;
uniform float u_cameraZoom;
uniform float u_devicePixelRatio;
out vec4 v_color;
void main() {
  gl_Position = u_viewProjection * vec4(a_position, 0.0, 1.0);
  gl_PointSize = max(1.0, a_size * u_cameraZoom * u_devicePixelRatio);
  v_color = a_color;
}`;

const OVERLAY_FRAGMENT_SHADER = `#version 300 es
precision mediump float;
uniform bool u_disc;
in vec4 v_color;
out vec4 outColor;
void main() {
  if (u_disc) {
    vec2 point = gl_PointCoord * 2.0 - 1.0;
    if (dot(point, point) > 1.0) discard;
  }
  outColor = v_color;
}`;

const EFFECT_VERTEX_SHADER = `#version 300 es
layout(location=0) in vec3 a_position;
layout(location=1) in vec2 a_uv;
uniform mat4 u_viewProjection;
uniform vec3 u_origin;
uniform vec3 u_scale;
uniform vec2 u_dir;
out vec3 v_local;
out vec2 v_uv;
void main() {
  vec2 dir = dot(u_dir, u_dir) > 0.0001 ? normalize(u_dir) : vec2(1.0, 0.0);
  vec2 perp = vec2(-dir.y, dir.x);
  vec3 world = u_origin + vec3(
    dir * a_position.x * u_scale.x + perp * a_position.y * u_scale.y,
    a_position.z * u_scale.z);
  v_local = a_position;
  v_uv = a_uv;
  gl_Position = u_viewProjection * vec4(world, 1.0);
}`;

const EFFECT_FRAGMENT_SHADER = `#version 300 es
precision mediump float;
precision mediump int;
uniform vec3 u_color;
uniform int u_kind;
in vec3 v_local;
in vec2 v_uv;
out vec4 outColor;

float hexCell(vec2 p) {
  const vec2 s = vec2(1.0, 1.7320508);
  vec2 a = mod(p, s) - 0.5 * s;
  vec2 b = mod(p + 0.5 * s, s) - 0.5 * s;
  return min(dot(a, a), dot(b, b));
}

void main() {
  vec3 n = normalize(v_local);
  float rim = pow(1.0 - abs(n.z), 1.35);
  if (u_kind == 0) {
    float edge = 1.0 - smoothstep(0.016, 0.042, hexCell(n.xy * 7.5));
    float alpha = 0.20 + rim * 0.48 + edge * 0.42;
    vec3 color = mix(u_color, vec3(1.0), edge * 0.32 + rim * 0.22);
    if (!gl_FrontFacing) alpha *= 0.55;
    outColor = vec4(color, clamp(alpha, 0.0, 0.88));
  } else if (u_kind == 1) {
    float ring = smoothstep(0.78, 1.0, length(v_local.xy));
    outColor = vec4(mix(u_color, vec3(1.0), ring * 0.45), 0.28 + ring * 0.5);
  } else if (u_kind == 2) {
    float core = exp(-length(v_local.xy) * 1.6);
    outColor = vec4(mix(u_color, vec3(1.0, 0.86, 0.42), core), 0.18 + rim * 0.5 + core * 0.32);
  } else {
    float glow = 0.55 + (1.0 - clamp(length(v_local.yz), 0.0, 1.0)) * 0.35;
    outColor = vec4(mix(u_color, vec3(1.0), 0.22 + v_uv.y * 0.08), glow);
  }
}`;

const OVERLAY_STRIDE_FLOATS = 7;
const MAX_OVERLAY_LINE_VERTICES = 2048;
const MAX_OVERLAY_POINT_VERTICES = 2048;
const OVERLAY_POINT_FLOAT_OFFSET = MAX_OVERLAY_LINE_VERTICES * OVERLAY_STRIDE_FLOATS;

export interface AnimatedFighter {
  slot: number;
  port: number;
  follower: boolean;
  model: ModelAsset;
  boneRows: Float32Array;
  poseVersion: number;
  label: string;
  rootX: number;
  rootY: number;
  facing: number;
  visible: boolean;
  actionState: number;
  actionName: string | null;
  characterId: number;
  shield: number;
  percent: number;
  stocks: number;
}

export interface DynamicStageState {
  fodLeft: number;
  fodRight: number;
  whispyDirection: number;
  stadiumEvent: number;
  stadiumType: number;
}

export interface WebGLSceneSource {
  stageSections: readonly ModelAsset[];
  stageScale: number;
  fighters: readonly AnimatedFighter[];
  items: readonly TimelineItem[];
  itemStart: number;
  itemEnd: number;
  stageState: DynamicStageState;
  /** Skinned stage sections (e.g. FoD's moving platforms) driven per frame. */
  stageAnimated?: readonly {
    model: ModelAsset;
    boneRows: Float32Array;
    poseVersion: number;
  }[];
  effectModels?: EffectModelBank;
}

export interface RendererCapabilities {
  maxTextureSize: number;
  maxVertexTextureUnits: number;
  maxVertexAttributes: number;
  floatTexture: boolean;
}

interface MeshUniforms {
  viewProjection: WebGLUniformLocation;
  replayRoot: WebGLUniformLocation;
  facing: WebGLUniformLocation;
  modelScale: WebGLUniformLocation;
  profile: WebGLUniformLocation;
  skinned: WebGLUniformLocation;
  billboard: WebGLUniformLocation;
  ray: WebGLUniformLocation;
  viewBillboard: WebGLUniformLocation;
  cameraRight: WebGLUniformLocation;
  cameraUp: WebGLUniformLocation;
  dir: WebGLUniformLocation;
  texture: WebGLUniformLocation;
  detail: WebGLUniformLocation;
  boneMatrices: WebGLUniformLocation;
  tint: WebGLUniformLocation;
  mirrorMask: WebGLUniformLocation;
}

interface Programs {
  mesh: WebGLProgram;
  backdrop: WebGLProgram;
  stars: WebGLProgram;
  overlay: WebGLProgram;
  effect: WebGLProgram;
  uniforms: MeshUniforms;
  starPointSize: WebGLUniformLocation;
  overlayUniforms: {
    viewProjection: WebGLUniformLocation;
    cameraZoom: WebGLUniformLocation;
    devicePixelRatio: WebGLUniformLocation;
    disc: WebGLUniformLocation;
  };
  effectUniforms: {
    viewProjection: WebGLUniformLocation;
    origin: WebGLUniformLocation;
    scale: WebGLUniformLocation;
    dir: WebGLUniformLocation;
    color: WebGLUniformLocation;
    kind: WebGLUniformLocation;
  };
}

interface GpuEffectMesh {
  vao: WebGLVertexArrayObject;
  buffers: WebGLBuffer[];
  indexCount: number;
  gpuBytes: number;
}

interface GpuMesh {
  vao: WebGLVertexArrayObject;
  buffers: WebGLBuffer[];
  textures: WebGLTexture[];
  boneTexture: WebGLTexture | null;
  uploadedPoseVersion: number;
  model: ModelAsset;
  minDepth: number;
  maxDepth: number;
  gpuBytes: number;
  xyRadius: number;
  xyExtent: number;
}

function compileShader(gl: WebGL2RenderingContext, type: number, source: string): WebGLShader {
  const shader = gl.createShader(type);
  if (!shader) throw new Error('WebGL2 could not allocate a shader');
  gl.shaderSource(shader, source);
  gl.compileShader(shader);
  if (!gl.getShaderParameter(shader, gl.COMPILE_STATUS)) {
    const message = gl.getShaderInfoLog(shader) || 'unknown shader error';
    gl.deleteShader(shader);
    throw new Error(`WebGL2 shader compilation failed: ${message}`);
  }
  return shader;
}

function createProgram(gl: WebGL2RenderingContext, vertex: string, fragment: string): WebGLProgram {
  const vs = compileShader(gl, gl.VERTEX_SHADER, vertex);
  const fs = compileShader(gl, gl.FRAGMENT_SHADER, fragment);
  const program = gl.createProgram();
  if (!program) throw new Error('WebGL2 could not allocate a program');
  gl.attachShader(program, vs);
  gl.attachShader(program, fs);
  gl.linkProgram(program);
  gl.deleteShader(vs);
  gl.deleteShader(fs);
  if (!gl.getProgramParameter(program, gl.LINK_STATUS)) {
    const message = gl.getProgramInfoLog(program) || 'unknown link error';
    gl.deleteProgram(program);
    throw new Error(`WebGL2 program link failed: ${message}`);
  }
  return program;
}

function uniform(gl: WebGL2RenderingContext, program: WebGLProgram, name: string): WebGLUniformLocation {
  const location = gl.getUniformLocation(program, name);
  if (!location) throw new Error(`WebGL2 shader is missing ${name}`);
  return location;
}

function createBuffer(gl: WebGL2RenderingContext, target: number, data: ArrayBufferView<ArrayBufferLike>): WebGLBuffer {
  const buffer = gl.createBuffer();
  if (!buffer) throw new Error('WebGL2 could not allocate a buffer');
  gl.bindBuffer(target, buffer);
  gl.bufferData(target, data as unknown as BufferSource, gl.STATIC_DRAW);
  return buffer;
}

export class WebGL2Renderer implements Renderer {
  readonly canvas: HTMLCanvasElement;
  readonly capabilities: RendererCapabilities;
  private gl: WebGL2RenderingContext;
  private programs: Programs | null = null;
  private whiteTexture: WebGLTexture | null = null;
  private starVao: WebGLVertexArrayObject | null = null;
  private starBuffer: WebGLBuffer | null = null;
  private overlayVao: WebGLVertexArrayObject | null = null;
  private overlayBuffer: WebGLBuffer | null = null;
  private shieldMesh: GpuEffectMesh | null = null;
  private shineMesh: GpuEffectMesh | null = null;
  private laserMesh: GpuEffectMesh | null = null;
  private readonly overlayData = new Float32Array(
    (MAX_OVERLAY_LINE_VERTICES + MAX_OVERLAY_POINT_VERTICES) * OVERLAY_STRIDE_FLOATS);
  private overlayLineVertices = 0;
  private overlayPointVertices = 0;
  private overlayTruncatedValue = false;
  private stageMeshes: GpuMesh[] = [];
  private stageAnimatedMeshes: GpuMesh[] = [];
  private fighterMeshes: GpuMesh[] = [];
  private extractedMeshes = new Map<string, GpuMesh>();
  private source: WebGLSceneSource = {
    stageSections: [], stageScale: 1, fighters: [], items: [], itemStart: 0, itemEnd: 0,
    stageState: { fodLeft: Number.NaN, fodRight: Number.NaN, whispyDirection: -1, stadiumEvent: -1, stadiumType: -1 },
  };
  private size: RenderSize = { width: 960, height: 720, devicePixelRatio: 1 };
  private lastCamera: CameraState | null = null;
  private readonly viewProjection = new Float32Array(16);
  private readonly cameraRight = new Float32Array([1, 0, 0]);
  private readonly cameraUp = new Float32Array([0, 1, 0]);
  private disposed = false;
  private lost = false;
  private readonly status: (message: string) => void;
  private readonly handleLost: (event: Event) => void;
  private readonly handleRestored: () => void;

  constructor(canvas: HTMLCanvasElement, status: (message: string) => void = () => undefined) {
    this.canvas = canvas;
    this.status = status;
    const gl = canvas.getContext('webgl2', {
      alpha: false, antialias: false, depth: true, premultipliedAlpha: false,
      preserveDrawingBuffer: true,
    });
    if (!gl) throw new Error('WebGL2 is unavailable. Open /?renderer=software for the C viewer.');
    this.gl = gl;
    this.capabilities = this.checkCapabilities();
    this.handleLost = (event: Event) => {
      event.preventDefault();
      this.lost = true;
      // Every object from the previous context generation is already invalid.
      // Drop the handles without issuing delete calls against the restored
      // generation; the retained CPU scene is enough to rebuild all of them.
      this.abandonGpuResources();
      this.status('WebGL2 context lost; waiting for the browser to restore it…');
    };
    this.handleRestored = () => {
      if (this.disposed) return;
      this.lost = false;
      try {
        this.initialize();
        this.setScene(this.source);
        if (this.lastCamera) this.draw(this.lastCamera);
        this.status('WebGL2 context restored');
      } catch (error) {
        this.status(error instanceof Error ? error.message : String(error));
      }
    };
    canvas.addEventListener('webglcontextlost', this.handleLost);
    canvas.addEventListener('webglcontextrestored', this.handleRestored);
  }

  private checkCapabilities(): RendererCapabilities {
    const gl = this.gl;
    const maxTextureSize = gl.getParameter(gl.MAX_TEXTURE_SIZE) as number;
    const maxVertexTextureUnits = gl.getParameter(gl.MAX_VERTEX_TEXTURE_IMAGE_UNITS) as number;
    const maxVertexAttributes = gl.getParameter(gl.MAX_VERTEX_ATTRIBS) as number;
    if (maxTextureSize < 2048 || maxVertexTextureUnits < 1 || maxVertexAttributes < 8) {
      throw new Error(`WebGL2 capability gate failed (texture ${maxTextureSize}, vertex textures ${maxVertexTextureUnits}, attributes ${maxVertexAttributes})`);
    }
    const probe = gl.createTexture();
    if (!probe) throw new Error('WebGL2 capability gate could not allocate a float texture');
    gl.bindTexture(gl.TEXTURE_2D, probe);
    while (gl.getError() !== gl.NO_ERROR) { /* clear stale context errors */ }
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA32F, 1, 1, 0, gl.RGBA, gl.FLOAT, new Float32Array(4));
    const floatTexture = gl.getError() === gl.NO_ERROR;
    gl.deleteTexture(probe);
    if (!floatTexture) throw new Error('WebGL2 capability gate failed: RGBA32F textures are unavailable');
    return { maxTextureSize, maxVertexTextureUnits, maxVertexAttributes, floatTexture };
  }

  initialize(): void {
    if (this.disposed) throw new Error('WebGL2 renderer has been disposed');
    this.destroyGpuResources();
    const gl = this.gl;
    let mesh: WebGLProgram | null = null;
    let backdrop: WebGLProgram | null = null;
    let stars: WebGLProgram | null = null;
    let overlay: WebGLProgram | null = null;
    let effect: WebGLProgram | null = null;
    try {
      mesh = createProgram(gl, MESH_VERTEX_SHADER, MESH_FRAGMENT_SHADER);
      backdrop = createProgram(gl, BACKDROP_VERTEX_SHADER, BACKDROP_FRAGMENT_SHADER);
      stars = createProgram(gl, STAR_VERTEX_SHADER, STAR_FRAGMENT_SHADER);
      overlay = createProgram(gl, OVERLAY_VERTEX_SHADER, OVERLAY_FRAGMENT_SHADER);
      effect = createProgram(gl, EFFECT_VERTEX_SHADER, EFFECT_FRAGMENT_SHADER);
      this.programs = {
        mesh, backdrop, stars, overlay, effect,
        uniforms: {
          viewProjection: uniform(gl, mesh, 'u_viewProjection'),
          replayRoot: uniform(gl, mesh, 'u_replayRoot'),
          facing: uniform(gl, mesh, 'u_facing'),
          modelScale: uniform(gl, mesh, 'u_modelScale'),
          profile: uniform(gl, mesh, 'u_profile'),
          skinned: uniform(gl, mesh, 'u_skinned'),
          billboard: uniform(gl, mesh, 'u_billboard'),
          ray: uniform(gl, mesh, 'u_ray'),
          viewBillboard: uniform(gl, mesh, 'u_viewBillboard'),
          cameraRight: uniform(gl, mesh, 'u_cameraRight'),
          cameraUp: uniform(gl, mesh, 'u_cameraUp'),
          dir: uniform(gl, mesh, 'u_dir'),
          texture: uniform(gl, mesh, 'u_texture'),
          detail: uniform(gl, mesh, 'u_detail'),
          boneMatrices: uniform(gl, mesh, 'u_boneMatrices'),
          tint: uniform(gl, mesh, 'u_tint'),
          mirrorMask: uniform(gl, mesh, 'u_mirrorMask'),
        },
        starPointSize: uniform(gl, stars, 'u_pointSize'),
        overlayUniforms: {
          viewProjection: uniform(gl, overlay, 'u_viewProjection'),
          cameraZoom: uniform(gl, overlay, 'u_cameraZoom'),
          devicePixelRatio: uniform(gl, overlay, 'u_devicePixelRatio'),
          disc: uniform(gl, overlay, 'u_disc'),
        },
        effectUniforms: {
          viewProjection: uniform(gl, effect, 'u_viewProjection'),
          origin: uniform(gl, effect, 'u_origin'),
          scale: uniform(gl, effect, 'u_scale'),
          dir: uniform(gl, effect, 'u_dir'),
          color: uniform(gl, effect, 'u_color'),
          kind: uniform(gl, effect, 'u_kind'),
        },
      };
      this.whiteTexture = this.uploadTexture(1, 1, new Uint8Array([255, 255, 255, 255]));
      this.createStars();
      this.createOverlayBuffer();
      this.createEffectMeshes();
      gl.clearColor(5 / 255, 7 / 255, 18 / 255, 1);
    } catch (error) {
      if (this.programs) this.destroyGpuResources();
      else {
        if (mesh) gl.deleteProgram(mesh);
        if (backdrop) gl.deleteProgram(backdrop);
        if (stars) gl.deleteProgram(stars);
        if (overlay) gl.deleteProgram(overlay);
        if (effect) gl.deleteProgram(effect);
      }
      throw error;
    }
  }

  resize(size: RenderSize): void {
    this.size = {
      width: Math.max(1, size.width),
      height: Math.max(1, size.height),
      devicePixelRatio: Math.min(2, Math.max(1, size.devicePixelRatio)),
    };
    const pixelWidth = Math.round(this.size.width * this.size.devicePixelRatio);
    const pixelHeight = Math.round(this.size.height * this.size.devicePixelRatio);
    if (this.canvas.width !== pixelWidth || this.canvas.height !== pixelHeight) {
      this.canvas.width = pixelWidth;
      this.canvas.height = pixelHeight;
    }
    if (this.lastCamera) this.draw(this.lastCamera);
  }

  setScene(source: WebGLSceneSource): void {
    this.source = source;
    if (!this.programs || this.lost) return;
    this.destroyMeshes();
    try {
      this.stageMeshes = source.stageSections.map(model => this.uploadMesh(model, false));
      this.stageAnimatedMeshes = (source.stageAnimated ?? []).map(
        entry => this.uploadMesh(entry.model, true));
      this.fighterMeshes = source.fighters.map(fighter => this.uploadMesh(fighter.model, true));
      const bank = source.effectModels;
      if (bank) {
        for (const [alias, model] of Object.entries(bank.byAlias)) {
          this.extractedMeshes.set(`alias:${alias}`, this.uploadMesh(model, false, 'linear'));
        }
        for (const [id, model] of Object.entries(bank.byItem)) {
          this.extractedMeshes.set(`item:${id}`, this.uploadMesh(model, false, 'linear'));
        }
      }
    } catch (error) {
      this.destroyMeshes();
      throw error;
    }
  }

  render(scene: SceneSnapshot): void {
    this.draw(scene.camera);
  }

  draw(camera: CameraState): void {
    this.lastCamera = { ...camera };
    if (this.disposed || this.lost || !this.programs) return;
    cameraViewProjection(camera, { width: this.size.width, height: this.size.height }, this.viewProjection);
    const axes = cameraViewAxes(camera);
    this.cameraRight.set(axes.right);
    this.cameraUp.set(axes.up);
    const gl = this.gl;
    gl.viewport(0, 0, this.canvas.width, this.canvas.height);
    gl.disable(gl.DEPTH_TEST);
    gl.disable(gl.BLEND);
    gl.useProgram(this.programs.backdrop);
    gl.bindVertexArray(null);
    gl.drawArrays(gl.TRIANGLES, 0, 3);
    this.drawStars();

    gl.enable(gl.DEPTH_TEST);
    gl.depthFunc(gl.LESS);
    gl.depthMask(true);
    gl.enable(gl.BLEND);
    gl.blendFunc(gl.SRC_ALPHA, gl.ONE_MINUS_SRC_ALPHA);
    gl.clear(gl.DEPTH_BUFFER_BIT);
    for (const mesh of this.stageMeshes) {
      this.drawMesh(mesh, camera, false, 0, 0, 1, this.source.stageScale);
    }
    const animatedStages = this.source.stageAnimated ?? [];
    for (let index = 0; index < this.stageAnimatedMeshes.length; index++) {
      const mesh = this.stageAnimatedMeshes[index];
      const entry = animatedStages[index];
      if (!entry) continue;
      if (mesh.uploadedPoseVersion !== entry.poseVersion) {
        this.uploadBoneRows(mesh, entry.boneRows);
        mesh.uploadedPoseVersion = entry.poseVersion;
      }
      this.drawMesh(mesh, camera, false, 0, 0, 1, this.source.stageScale, { skinnedStage: true });
    }
    for (let index = 0; index < this.fighterMeshes.length; index++) {
      const mesh = this.fighterMeshes[index];
      const fighter = this.source.fighters[index];
      if (!fighter?.visible) continue;
      if (mesh.uploadedPoseVersion !== fighter.poseVersion) {
        this.uploadBoneRows(mesh, fighter.boneRows);
        mesh.uploadedPoseVersion = fighter.poseVersion;
      }
      this.drawMesh(mesh, camera, true, fighter.rootX, fighter.rootY, fighter.facing, 1);
    }
    this.drawEffects();
    this.drawOverlays(camera);
    gl.bindVertexArray(null);
  }

  get gpuBytes(): number {
    return this.stageMeshes.reduce((sum, mesh) => sum + mesh.gpuBytes, 0)
      + this.stageAnimatedMeshes.reduce((sum, mesh) => sum + mesh.gpuBytes, 0)
      + this.fighterMeshes.reduce((sum, mesh) => sum + mesh.gpuBytes, 0)
      + [...this.extractedMeshes.values()].reduce((sum, mesh) => sum + mesh.gpuBytes, 0)
      + this.overlayData.byteLength
      + (this.shieldMesh?.gpuBytes ?? 0)
      + (this.shineMesh?.gpuBytes ?? 0)
      + (this.laserMesh?.gpuBytes ?? 0);
  }

  get overlayTruncated(): boolean { return this.overlayTruncatedValue; }

  dispose(): void {
    if (this.disposed) return;
    this.disposed = true;
    this.canvas.removeEventListener('webglcontextlost', this.handleLost);
    this.canvas.removeEventListener('webglcontextrestored', this.handleRestored);
    this.destroyGpuResources();
  }

  private uploadTexture(width: number, height: number, rgba: Uint8Array, filter: 'nearest' | 'linear' = 'nearest'): WebGLTexture {
    if (width > this.capabilities.maxTextureSize || height > this.capabilities.maxTextureSize) {
      throw new Error(`texture ${width}×${height} exceeds this GPU's ${this.capabilities.maxTextureSize}px limit`);
    }
    const gl = this.gl;
    const texture = gl.createTexture();
    if (!texture) throw new Error('WebGL2 could not allocate a texture');
    const mag = filter === 'linear' ? gl.LINEAR : gl.NEAREST;
    gl.bindTexture(gl.TEXTURE_2D, texture);
    gl.pixelStorei(gl.UNPACK_ALIGNMENT, 1);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, mag);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, mag);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA8, width, height, 0, gl.RGBA, gl.UNSIGNED_BYTE, rgba);
    return texture;
  }

  private uploadMesh(model: ModelAsset, profile: boolean, filter: 'nearest' | 'linear' = 'nearest'): GpuMesh {
    const gl = this.gl;
    const positions = transformBindPose(model);
    const bounds = positionBounds(positions);
    const vao = gl.createVertexArray();
    if (!vao) throw new Error('WebGL2 could not allocate a vertex array');
    const buffers: WebGLBuffer[] = [];
    const textures: WebGLTexture[] = [];
    try {
      gl.bindVertexArray(vao);
      buffers.push(createBuffer(gl, gl.ARRAY_BUFFER, profile ? model.positions : positions));
      gl.enableVertexAttribArray(0);
      gl.vertexAttribPointer(0, 3, gl.FLOAT, false, 0, 0);
      buffers.push(createBuffer(gl, gl.ARRAY_BUFFER, model.uvs));
      gl.enableVertexAttribArray(1);
      gl.vertexAttribPointer(1, 2, gl.FLOAT, false, 0, 0);
      buffers.push(createBuffer(gl, gl.ARRAY_BUFFER, model.colors));
      gl.enableVertexAttribArray(2);
      gl.vertexAttribPointer(2, 4, gl.UNSIGNED_BYTE, true, 0, 0);
      if (profile) {
        buffers.push(createBuffer(gl, gl.ARRAY_BUFFER, model.weights));
        gl.enableVertexAttribArray(3);
        gl.vertexAttribPointer(3, 4, gl.FLOAT, false, 0, 0);
      }
      // Integer shader inputs must have an integer-bound attribute even when
      // the unskinned stage branch does not read it.
      buffers.push(createBuffer(gl, gl.ARRAY_BUFFER, model.boneIndices));
      gl.enableVertexAttribArray(4);
      gl.vertexAttribIPointer(4, 4, gl.UNSIGNED_SHORT, 0, 0);
      buffers.push(createBuffer(gl, gl.ELEMENT_ARRAY_BUFFER, model.indices));
      for (const texture of model.textures) textures.push(this.uploadTexture(texture.width, texture.height, texture.rgba, filter));
      const textureBytes = model.textures.reduce((sum, texture) => sum + texture.rgba.byteLength, 0);
      const boneTexture = profile ? this.createBoneTexture(model.boneCount) : null;
      return {
        vao, buffers, textures, boneTexture, uploadedPoseVersion: -1, model,
        minDepth: profile ? Math.min(-100, bounds[0]) : bounds[2],
        maxDepth: profile ? Math.max(100, bounds[3]) : bounds[5],
        xyRadius: xyRadius(positions),
        xyExtent: xyExtent(positions),
        gpuBytes: (profile ? model.positions.byteLength + model.weights.byteLength : positions.byteLength) + model.boneIndices.byteLength
          + model.uvs.byteLength + model.colors.byteLength + model.indices.byteLength + textureBytes
          + (profile ? model.boneCount * 24 * 4 : 0),
      };
    } catch (error) {
      gl.deleteVertexArray(vao);
      for (const buffer of buffers) gl.deleteBuffer(buffer);
      for (const texture of textures) gl.deleteTexture(texture);
      throw error;
    }
  }

  private drawMesh(
    mesh: GpuMesh, camera: CameraState, profile: boolean,
    rootX: number, rootY: number, facing: number, modelScale: number,
    extra?: { billboard?: boolean; ray?: boolean; viewBillboard?: boolean; mirrorMask?: boolean; dirX?: number; dirY?: number; tint?: readonly [number, number, number, number]; skinnedStage?: boolean },
  ): void {
    if (!this.programs || !this.whiteTexture) return;
    const gl = this.gl;
    const u = this.programs.uniforms;
    gl.useProgram(this.programs.mesh);
    gl.bindVertexArray(mesh.vao);
    gl.uniformMatrix4fv(u.viewProjection, false, this.viewProjection);
    gl.uniform2f(u.replayRoot, rootX, rootY);
    gl.uniform1f(u.facing, facing < 0 ? -1 : 1);
    gl.uniform1f(u.modelScale, modelScale);
    gl.uniform1i(u.profile, profile ? 1 : 0);
    gl.uniform1i(u.skinned, profile || extra?.skinnedStage ? 1 : 0);
    gl.uniform1i(u.billboard, extra?.billboard ? 1 : 0);
    gl.uniform1i(u.ray, extra?.ray ? 1 : 0);
    gl.uniform1i(u.viewBillboard, extra?.viewBillboard ? 1 : 0);
    gl.uniform1i(u.mirrorMask, extra?.mirrorMask ? 1 : 0);
    gl.uniform3fv(u.cameraRight, this.cameraRight);
    gl.uniform3fv(u.cameraUp, this.cameraUp);
    gl.uniform2f(u.dir, extra?.dirX ?? 1, extra?.dirY ?? 0);
    gl.uniform1i(u.texture, 0);
    gl.uniform1i(u.detail, 2);
    gl.uniform1i(u.boneMatrices, 1);
    gl.activeTexture(gl.TEXTURE1);
    gl.bindTexture(gl.TEXTURE_2D, mesh.boneTexture ?? this.whiteTexture);
    for (let index = 0; index < mesh.model.primitiveGroups.length; index++) {
      const group = mesh.model.primitiveGroups[index];
      const phong = index < mesh.model.phongs.length ? mesh.model.phongs[index] : null;
      if (extra?.tint) {
        gl.uniform4f(u.tint, extra.tint[0], extra.tint[1], extra.tint[2], extra.tint[3]);
      } else if (phong) {
        gl.uniform4f(u.tint, phong.diffuse[0] / 255, phong.diffuse[1] / 255, phong.diffuse[2] / 255,
          Math.min(1, Math.max(0, phong.alpha)) * phong.diffuse[3] / 255);
      } else {
        const flags = group.materialFlags;
        gl.uniform4f(u.tint, (150 + ((flags >>> 4) & 0x3f)) / 255,
          (170 + ((flags >>> 12) & 0x3f)) / 255,
          (200 + ((flags >>> 20) & 0x3f)) / 255, 1);
      }
      gl.activeTexture(gl.TEXTURE2);
      gl.bindTexture(gl.TEXTURE_2D, extra?.mirrorMask && mesh.textures.length > 1
        ? mesh.textures[1] : this.whiteTexture);
      gl.activeTexture(gl.TEXTURE0);
      gl.bindTexture(gl.TEXTURE_2D, group.textureIndex >= 0 ? mesh.textures[group.textureIndex] : this.whiteTexture);
      gl.drawElements(gl.TRIANGLES, group.indexLength, gl.UNSIGNED_SHORT, group.indexStart * 2);
    }
  }

  private createBoneTexture(boneCount: number): WebGLTexture {
    const gl = this.gl;
    if (boneCount > this.capabilities.maxTextureSize) {
      throw new Error(`bone texture height ${boneCount} exceeds this GPU's ${this.capabilities.maxTextureSize}px limit`);
    }
    const texture = gl.createTexture();
    if (!texture) throw new Error('WebGL2 could not allocate a bone texture');
    gl.bindTexture(gl.TEXTURE_2D, texture);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA32F, 6, boneCount, 0, gl.RGBA, gl.FLOAT, null);
    return texture;
  }

  private uploadBoneRows(mesh: GpuMesh, boneRows: Float32Array): void {
    if (!mesh.boneTexture) return;
    if (boneRows.length !== mesh.model.boneCount * 24) {
      throw new Error(`bone texture expected ${mesh.model.boneCount * 24} floats, got ${boneRows.length}`);
    }
    const gl = this.gl;
    gl.activeTexture(gl.TEXTURE1);
    gl.bindTexture(gl.TEXTURE_2D, mesh.boneTexture);
    gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, 0, 6, mesh.model.boneCount, gl.RGBA, gl.FLOAT, boneRows);
  }

  private createOverlayBuffer(): void {
    const gl = this.gl;
    const vao = gl.createVertexArray();
    const buffer = gl.createBuffer();
    if (!vao || !buffer) {
      if (vao) gl.deleteVertexArray(vao);
      if (buffer) gl.deleteBuffer(buffer);
      throw new Error('WebGL2 could not allocate the dynamic overlay buffer');
    }
    gl.bindVertexArray(vao);
    gl.bindBuffer(gl.ARRAY_BUFFER, buffer);
    gl.bufferData(gl.ARRAY_BUFFER, this.overlayData.byteLength, gl.DYNAMIC_DRAW);
    gl.enableVertexAttribArray(0);
    gl.vertexAttribPointer(0, 2, gl.FLOAT, false, OVERLAY_STRIDE_FLOATS * 4, 0);
    gl.enableVertexAttribArray(1);
    gl.vertexAttribPointer(1, 4, gl.FLOAT, false, OVERLAY_STRIDE_FLOATS * 4, 8);
    gl.enableVertexAttribArray(2);
    gl.vertexAttribPointer(2, 1, gl.FLOAT, false, OVERLAY_STRIDE_FLOATS * 4, 24);
    this.overlayVao = vao;
    this.overlayBuffer = buffer;
  }

  private writeOverlayVertex(
    vertex: number, x: number, y: number,
    red: number, green: number, blue: number, alpha: number, size: number,
  ): void {
    const offset = vertex * OVERLAY_STRIDE_FLOATS;
    this.overlayData[offset] = x;
    this.overlayData[offset + 1] = y;
    this.overlayData[offset + 2] = red;
    this.overlayData[offset + 3] = green;
    this.overlayData[offset + 4] = blue;
    this.overlayData[offset + 5] = alpha;
    this.overlayData[offset + 6] = size;
  }

  private pushOverlayLine(
    x0: number, y0: number, x1: number, y1: number,
    red: number, green: number, blue: number, alpha: number,
  ): void {
    if (this.overlayLineVertices + 2 > MAX_OVERLAY_LINE_VERTICES) {
      this.overlayTruncatedValue = true;
      return;
    }
    this.writeOverlayVertex(this.overlayLineVertices++, x0, y0, red, green, blue, alpha, 1);
    this.writeOverlayVertex(this.overlayLineVertices++, x1, y1, red, green, blue, alpha, 1);
  }

  private pushOverlayPoint(
    x: number, y: number, size: number,
    red: number, green: number, blue: number, alpha: number,
  ): void {
    if (this.overlayPointVertices >= MAX_OVERLAY_POINT_VERTICES) {
      this.overlayTruncatedValue = true;
      return;
    }
    this.writeOverlayVertex(MAX_OVERLAY_LINE_VERTICES + this.overlayPointVertices++,
      x, y, red, green, blue, alpha, size);
  }

  private createEffectMeshes(): void {
    this.shieldMesh = this.uploadEffectMesh(createUvSphere(20, 32));
    this.shineMesh = this.uploadEffectMesh(createHexPrism(0.35));
    this.laserMesh = this.uploadEffectMesh(createUnitCylinder(12));
  }

  private uploadEffectMesh(mesh: GeneratedMesh): GpuEffectMesh {
    const gl = this.gl;
    const vao = gl.createVertexArray();
    if (!vao) throw new Error('WebGL2 could not allocate an effect vertex array');
    const buffers: WebGLBuffer[] = [];
    try {
      gl.bindVertexArray(vao);
      buffers.push(createBuffer(gl, gl.ARRAY_BUFFER, mesh.positions));
      gl.enableVertexAttribArray(0);
      gl.vertexAttribPointer(0, 3, gl.FLOAT, false, 0, 0);
      buffers.push(createBuffer(gl, gl.ARRAY_BUFFER, mesh.uvs));
      gl.enableVertexAttribArray(1);
      gl.vertexAttribPointer(1, 2, gl.FLOAT, false, 0, 0);
      buffers.push(createBuffer(gl, gl.ELEMENT_ARRAY_BUFFER, mesh.indices));
      return {
        vao, buffers, indexCount: mesh.indices.length,
        gpuBytes: mesh.positions.byteLength + mesh.uvs.byteLength + mesh.indices.byteLength,
      };
    } catch (error) {
      gl.deleteVertexArray(vao);
      for (const buffer of buffers) gl.deleteBuffer(buffer);
      throw error;
    }
  }

  private deleteEffectMesh(mesh: GpuEffectMesh | null): void {
    if (!mesh) return;
    const gl = this.gl;
    gl.deleteVertexArray(mesh.vao);
    for (const buffer of mesh.buffers) gl.deleteBuffer(buffer);
  }

  private drawEffect(
    mesh: GpuEffectMesh, kind: number,
    originX: number, originY: number, originZ: number,
    scaleX: number, scaleY: number, scaleZ: number,
    dirX: number, dirY: number,
    red: number, green: number, blue: number,
  ): void {
    if (!this.programs) return;
    const gl = this.gl;
    const uniforms = this.programs.effectUniforms;
    gl.bindVertexArray(mesh.vao);
    gl.uniform3f(uniforms.origin, originX, originY, originZ);
    gl.uniform3f(uniforms.scale, scaleX, scaleY, scaleZ);
    gl.uniform2f(uniforms.dir, dirX, dirY);
    gl.uniform3f(uniforms.color, red, green, blue);
    gl.uniform1i(uniforms.kind, kind);
    gl.drawElements(gl.TRIANGLES, mesh.indexCount, gl.UNSIGNED_SHORT, 0);
  }

  private extractedAlias(alias: string): GpuMesh | undefined {
    return this.extractedMeshes.get(`alias:${alias}`);
  }

  private extractedItem(typeId: number): GpuMesh | undefined {
    return this.extractedMeshes.get(`item:${typeId}`);
  }

  private drawExtracted(
    mesh: GpuMesh, originX: number, originY: number, scale: number,
    dirX: number, dirY: number,
    extra?: { ray?: boolean; viewBillboard?: boolean; mirrorMask?: boolean; tint?: readonly [number, number, number, number] },
  ): void {
    if (!this.lastCamera) return;
    this.drawMesh(mesh, this.lastCamera, false, originX, originY, 1, scale, {
      billboard: !extra?.viewBillboard, ray: extra?.ray, viewBillboard: extra?.viewBillboard,
      mirrorMask: extra?.mirrorMask, dirX, dirY, tint: extra?.tint,
    });
  }

  private drawEffects(): void {
    if (!this.programs || !this.shieldMesh || !this.shineMesh || !this.laserMesh) return;
    const gl = this.gl;
    gl.enable(gl.DEPTH_TEST);
    gl.depthFunc(gl.LESS);
    gl.depthMask(false);
    gl.enable(gl.BLEND);
    gl.disable(gl.CULL_FACE);

    let effectBound = false;
    const bindEffectProgram = (): void => {
      if (effectBound || !this.programs) return;
      gl.useProgram(this.programs.effect);
      gl.uniformMatrix4fv(this.programs.effectUniforms.viewProjection, false, this.viewProjection);
      effectBound = true;
    };

    gl.blendFunc(gl.SRC_ALPHA, gl.ONE_MINUS_SRC_ALPHA);
    const extractedShield = this.extractedAlias('shield');
    for (const fighter of this.source.fighters) {
      if (!fighter.visible || fighter.follower || !isShieldState(fighter.actionState, fighter.shield)) continue;
      const radius = shieldRadius(fighter.shield);
      const color = portShieldColor(fighter.port);
      if (extractedShield) {
        const extent = extractedShield.xyExtent > 0.05 ? extractedShield.xyExtent : extractedShield.xyRadius;
        this.drawExtracted(extractedShield, fighter.rootX, fighter.rootY + SHIELD_Y_OFFSET,
          radius / Math.max(extent, 0.05), fighter.facing, 0,
          { viewBillboard: true, mirrorMask: true, tint: [color[0], color[1], color[2], 0.72] });
      } else {
        bindEffectProgram();
        this.drawEffect(this.shieldMesh, EffectKind.Shield,
          fighter.rootX, fighter.rootY + SHIELD_Y_OFFSET, 0,
          radius, radius, radius, 1, 0, color[0], color[1], color[2]);
      }
    }

    gl.blendFunc(gl.SRC_ALPHA, gl.ONE);
    for (const fighter of this.source.fighters) {
      if (!fighter.visible || !isSpacie(fighter.characterId)) continue;
      if (isShineAction(fighter.actionName)) {
        const start = /Start/i.test(fighter.actionName ?? '');
        const shineMesh = this.extractedAlias(start ? 'shine-start' : 'shine')
          ?? this.extractedAlias('shine');
        if (shineMesh) {
          this.drawExtracted(shineMesh, fighter.rootX, fighter.rootY + 8,
            fitEffectScale(shineMesh.xyRadius, SHINE_RADIUS), fighter.facing, 0);
        } else {
          bindEffectProgram();
          this.drawEffect(this.shineMesh, EffectKind.Shine,
            fighter.rootX, fighter.rootY + 8, 0,
            SHINE_RADIUS, SHINE_RADIUS, SHINE_RADIUS, 1, 0,
            SHINE_COLOR[0], SHINE_COLOR[1], SHINE_COLOR[2]);
        }
      }
      if (isFirefoxAction(fighter.actionName)) {
        const hold = /Hold/i.test(fighter.actionName ?? '');
        const fireMesh = this.extractedAlias(hold ? 'firefox-charge' : 'firefox')
          ?? this.extractedAlias('firefox') ?? this.extractedAlias('firefox-charge');
        if (fireMesh) {
          this.drawExtracted(fireMesh, fighter.rootX, fighter.rootY + 10,
            fitEffectScale(fireMesh.xyRadius, FIRE_RADIUS), fighter.facing, 0);
        } else {
          bindEffectProgram();
          this.drawEffect(this.shieldMesh, EffectKind.Fire,
            fighter.rootX, fighter.rootY + 10, 0,
            FIRE_RADIUS, FIRE_RADIUS, FIRE_RADIUS, 1, 0,
            FIRE_COLOR[0], FIRE_COLOR[1], FIRE_COLOR[2]);
        }
      }
    }

    const itemStart = Math.max(0, Math.min(this.source.items.length, this.source.itemStart));
    const itemEnd = Math.max(itemStart, Math.min(this.source.items.length, this.source.itemEnd));
    for (let index = itemStart; index < itemEnd; index++) {
      const item = this.source.items[index];
      const foxLaser = isFoxLaser(item.typeId);
      const falcoLaser = isFalcoLaser(item.typeId);
      const extractedItem = this.extractedItem(item.typeId);
      const dirX = item.velocityX || item.facing, dirY = item.velocityY;
      if (foxLaser || falcoLaser) {
        const color = foxLaser ? FOX_LASER_COLOR : FALCO_LASER_COLOR;
        const length = laserLength(item.velocityX, item.velocityY);
        if (extractedItem) {
          this.drawExtracted(extractedItem, item.x, item.y, 1, dirX, dirY, { ray: true });
        } else {
          bindEffectProgram();
          this.drawEffect(this.laserMesh, EffectKind.Laser,
            item.x, item.y, 0, length, LASER_THICKNESS, LASER_THICKNESS,
            dirX, dirY, color[0], color[1], color[2]);
        }
      } else if (isFoxIllusion(item.typeId) || isFalcoPhantasm(item.typeId)) {
        const color = isFoxIllusion(item.typeId) ? FOX_LASER_COLOR : FALCO_LASER_COLOR;
        const length = Math.max(16, laserLength(item.velocityX, item.velocityY) * 1.4);
        if (extractedItem) {
          this.drawExtracted(extractedItem, item.x, item.y, 1, dirX, dirY, { ray: true });
        } else {
          bindEffectProgram();
          this.drawEffect(this.laserMesh, EffectKind.Laser,
            item.x, item.y, 0, length, 3.2, 3.2,
            dirX, dirY, color[0], color[1], color[2]);
        }
      }
    }
    gl.blendFunc(gl.SRC_ALPHA, gl.ONE_MINUS_SRC_ALPHA);
    gl.depthMask(true);
  }

  private drawOverlays(camera: CameraState): void {
    if (!this.programs || !this.overlayVao || !this.overlayBuffer) return;
    this.overlayLineVertices = 0;
    this.overlayPointVertices = 0;
    this.overlayTruncatedValue = false;
    const state = this.source.stageState;
    if (Number.isFinite(state.fodLeft))
      this.pushOverlayLine(-49.5, state.fodLeft, -21, state.fodLeft, 0.35, 0.82, 1, 0.8);
    if (Number.isFinite(state.fodRight))
      this.pushOverlayLine(21, state.fodRight, 49.5, state.fodRight, 0.35, 0.82, 1, 0.8);
    if (state.whispyDirection >= 0) {
      const direction = state.whispyDirection === 1 ? 1 : -1;
      this.pushOverlayLine(-72, 8, -72 + direction * 12, 8, 0.55, 1, 0.68, 0.75);
    }
    if (state.stadiumEvent >= 0)
      this.pushOverlayLine(-65, -2, 65, -2, 1, 0.72, 0.28, 0.55);

    const itemStart = Math.max(0, Math.min(this.source.items.length, this.source.itemStart));
    const itemEnd = Math.max(itemStart, Math.min(this.source.items.length, this.source.itemEnd));
    for (let index = itemStart; index < itemEnd; index++) {
      const item = this.source.items[index];
      if (isFoxLaser(item.typeId) || isFalcoLaser(item.typeId) ||
          isFoxIllusion(item.typeId) || isFalcoPhantasm(item.typeId)) {
        continue;
      } else if (item.typeId === 106 || item.typeId === 107) {
        this.pushOverlayPoint(item.x, item.y, 9 / camera.zoom, 0.58, 0.88, 1, 0.88);
      } else {
        this.pushOverlayPoint(item.x, item.y, 8 / camera.zoom, 1, 240 / 255, 170 / 255, 220 / 255);
      }
    }

    const gl = this.gl;
    gl.bindVertexArray(this.overlayVao);
    gl.bindBuffer(gl.ARRAY_BUFFER, this.overlayBuffer);
    if (this.overlayLineVertices)
      gl.bufferSubData(gl.ARRAY_BUFFER, 0, this.overlayData, 0,
        this.overlayLineVertices * OVERLAY_STRIDE_FLOATS);
    if (this.overlayPointVertices)
      gl.bufferSubData(gl.ARRAY_BUFFER, OVERLAY_POINT_FLOAT_OFFSET * 4, this.overlayData,
        OVERLAY_POINT_FLOAT_OFFSET, this.overlayPointVertices * OVERLAY_STRIDE_FLOATS);
    gl.disable(gl.DEPTH_TEST);
    gl.depthMask(false);
    gl.enable(gl.BLEND);
    gl.blendFunc(gl.SRC_ALPHA, gl.ONE_MINUS_SRC_ALPHA);
    gl.useProgram(this.programs.overlay);
    const uniforms = this.programs.overlayUniforms;
    gl.uniformMatrix4fv(uniforms.viewProjection, false, this.viewProjection);
    gl.uniform1f(uniforms.cameraZoom, camera.zoom);
    gl.uniform1f(uniforms.devicePixelRatio, this.size.devicePixelRatio);
    if (this.overlayLineVertices) {
      gl.uniform1i(uniforms.disc, 0);
      gl.drawArrays(gl.LINES, 0, this.overlayLineVertices);
    }
    if (this.overlayPointVertices) {
      gl.uniform1i(uniforms.disc, 1);
      gl.drawArrays(gl.POINTS, MAX_OVERLAY_LINE_VERTICES, this.overlayPointVertices);
    }
    gl.depthMask(true);
  }

  private createStars(): void {
    const gl = this.gl;
    const stars = new Float32Array(150 * 5);
    let seed = 0x4d454c45;
    for (let i = 0; i < 150; i++) {
      seed = (Math.imul(seed, 1664525) + 1013904223) >>> 0;
      const x = seed % 960;
      seed = (Math.imul(seed, 1664525) + 1013904223) >>> 0;
      const y = seed % 540;
      const glow = 105 + (seed >>> 24) / 2;
      const offset = i * 5;
      stars[offset] = x / 480 - 1;
      stars[offset + 1] = 1 - y / 360;
      stars[offset + 2] = glow / 255;
      stars[offset + 3] = glow / 255;
      stars[offset + 4] = (glow + (255 - glow) / 2) / 255;
    }
    const vao = gl.createVertexArray();
    if (!vao) throw new Error('WebGL2 could not allocate the backdrop vertex array');
    gl.bindVertexArray(vao);
    const buffer = createBuffer(gl, gl.ARRAY_BUFFER, stars);
    gl.enableVertexAttribArray(0);
    gl.vertexAttribPointer(0, 2, gl.FLOAT, false, 20, 0);
    gl.enableVertexAttribArray(1);
    gl.vertexAttribPointer(1, 3, gl.FLOAT, false, 20, 8);
    this.starVao = vao;
    this.starBuffer = buffer;
  }

  private drawStars(): void {
    if (!this.programs || !this.starVao) return;
    const gl = this.gl;
    gl.useProgram(this.programs.stars);
    gl.bindVertexArray(this.starVao);
    gl.uniform1f(this.programs.starPointSize, this.size.devicePixelRatio);
    gl.drawArrays(gl.POINTS, 0, 150);
  }

  private destroyMeshes(): void {
    for (const mesh of this.stageMeshes) this.deleteMesh(mesh);
    for (const mesh of this.stageAnimatedMeshes) this.deleteMesh(mesh);
    for (const mesh of this.fighterMeshes) this.deleteMesh(mesh);
    for (const mesh of this.extractedMeshes.values()) this.deleteMesh(mesh);
    this.stageMeshes = [];
    this.stageAnimatedMeshes = [];
    this.fighterMeshes = [];
    this.extractedMeshes.clear();
  }

  private deleteMesh(mesh: GpuMesh): void {
    const gl = this.gl;
    gl.deleteVertexArray(mesh.vao);
    for (const buffer of mesh.buffers) gl.deleteBuffer(buffer);
    for (const texture of mesh.textures) gl.deleteTexture(texture);
    if (mesh.boneTexture) gl.deleteTexture(mesh.boneTexture);
  }

  private destroyGpuResources(): void {
    const gl = this.gl;
    this.destroyMeshes();
    if (this.starVao) gl.deleteVertexArray(this.starVao);
    if (this.starBuffer) gl.deleteBuffer(this.starBuffer);
    if (this.overlayVao) gl.deleteVertexArray(this.overlayVao);
    if (this.overlayBuffer) gl.deleteBuffer(this.overlayBuffer);
    this.deleteEffectMesh(this.shieldMesh);
    this.deleteEffectMesh(this.shineMesh);
    this.deleteEffectMesh(this.laserMesh);
    if (this.whiteTexture) gl.deleteTexture(this.whiteTexture);
    if (this.programs) {
      gl.deleteProgram(this.programs.mesh);
      gl.deleteProgram(this.programs.backdrop);
      gl.deleteProgram(this.programs.stars);
      gl.deleteProgram(this.programs.overlay);
      gl.deleteProgram(this.programs.effect);
    }
    this.abandonGpuResources();
  }

  private abandonGpuResources(): void {
    this.stageMeshes = [];
    this.fighterMeshes = [];
    this.extractedMeshes.clear();
    this.starVao = null;
    this.starBuffer = null;
    this.overlayVao = null;
    this.overlayBuffer = null;
    this.shieldMesh = null;
    this.shineMesh = null;
    this.laserMesh = null;
    this.whiteTexture = null;
    this.programs = null;
  }
}
