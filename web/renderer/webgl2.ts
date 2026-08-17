import type { ModelAsset } from '../assets/model.js';
import type { TimelineItem } from '../replay/timeline.js';
import type { CameraState } from './camera.js';
import type { Renderer, RenderSize, SceneSnapshot } from './interface.js';
import { positionBounds, transformBindPose } from './static-pose.js';

const MESH_VERTEX_SHADER = `#version 300 es
layout(location=0) in vec3 a_position;
layout(location=1) in vec2 a_uv;
layout(location=2) in vec4 a_color;
layout(location=3) in vec4 a_weights;
layout(location=4) in uvec4 a_bones;

uniform vec2 u_cameraCenter;
uniform vec2 u_viewport;
uniform float u_cameraZoom;
uniform vec2 u_replayRoot;
uniform float u_facing;
uniform float u_modelScale;
uniform vec2 u_depthRange;
uniform bool u_profile;
uniform bool u_skinned;
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
  float horizontal = u_profile ? position.z * u_facing : position.x * u_modelScale;
  float vertical = position.y * u_modelScale;
  float depth = u_profile ? position.x : position.z;
  vec2 world = vec2(horizontal, vertical) + u_replayRoot;
  vec2 screen = (world - u_cameraCenter) * u_cameraZoom;
  float span = max(u_depthRange.y - u_depthRange.x, 0.0001);
  float depth01 = clamp((depth - u_depthRange.x) / span, 0.0, 1.0);
  gl_Position = vec4(screen * 2.0 / u_viewport, 0.999 * (1.0 - 2.0 * depth01), 1.0);
  v_uv = a_uv;
  v_color = a_color;
}`;

const MESH_FRAGMENT_SHADER = `#version 300 es
precision mediump float;
uniform sampler2D u_texture;
uniform vec4 u_tint;
in vec2 v_uv;
in vec4 v_color;
out vec4 outColor;
void main() {
  outColor = texture(u_texture, v_uv) * u_tint * v_color;
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
uniform vec2 u_cameraCenter;
uniform vec2 u_viewport;
uniform float u_cameraZoom;
uniform float u_devicePixelRatio;
out vec4 v_color;
void main() {
  vec2 screen = (a_position - u_cameraCenter) * u_cameraZoom;
  gl_Position = vec4(screen * 2.0 / u_viewport, 0.0, 1.0);
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
}

export interface RendererCapabilities {
  maxTextureSize: number;
  maxVertexTextureUnits: number;
  maxVertexAttributes: number;
  floatTexture: boolean;
}

interface MeshUniforms {
  cameraCenter: WebGLUniformLocation;
  viewport: WebGLUniformLocation;
  cameraZoom: WebGLUniformLocation;
  replayRoot: WebGLUniformLocation;
  facing: WebGLUniformLocation;
  modelScale: WebGLUniformLocation;
  depthRange: WebGLUniformLocation;
  profile: WebGLUniformLocation;
  skinned: WebGLUniformLocation;
  texture: WebGLUniformLocation;
  boneMatrices: WebGLUniformLocation;
  tint: WebGLUniformLocation;
}

interface Programs {
  mesh: WebGLProgram;
  backdrop: WebGLProgram;
  stars: WebGLProgram;
  overlay: WebGLProgram;
  uniforms: MeshUniforms;
  starPointSize: WebGLUniformLocation;
  overlayUniforms: {
    cameraCenter: WebGLUniformLocation;
    viewport: WebGLUniformLocation;
    cameraZoom: WebGLUniformLocation;
    devicePixelRatio: WebGLUniformLocation;
    disc: WebGLUniformLocation;
  };
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
  private readonly overlayData = new Float32Array(
    (MAX_OVERLAY_LINE_VERTICES + MAX_OVERLAY_POINT_VERTICES) * OVERLAY_STRIDE_FLOATS);
  private overlayLineVertices = 0;
  private overlayPointVertices = 0;
  private overlayTruncatedValue = false;
  private stageMeshes: GpuMesh[] = [];
  private fighterMeshes: GpuMesh[] = [];
  private source: WebGLSceneSource = {
    stageSections: [], stageScale: 1, fighters: [], items: [], itemStart: 0, itemEnd: 0,
    stageState: { fodLeft: Number.NaN, fodRight: Number.NaN, whispyDirection: -1, stadiumEvent: -1, stadiumType: -1 },
  };
  private size: RenderSize = { width: 960, height: 720, devicePixelRatio: 1 };
  private lastCamera: CameraState | null = null;
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
    if (!gl) throw new Error('WebGL2 is unavailable. The software viewer remains available without ?renderer=webgl2.');
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
    try {
      mesh = createProgram(gl, MESH_VERTEX_SHADER, MESH_FRAGMENT_SHADER);
      backdrop = createProgram(gl, BACKDROP_VERTEX_SHADER, BACKDROP_FRAGMENT_SHADER);
      stars = createProgram(gl, STAR_VERTEX_SHADER, STAR_FRAGMENT_SHADER);
      overlay = createProgram(gl, OVERLAY_VERTEX_SHADER, OVERLAY_FRAGMENT_SHADER);
      this.programs = {
        mesh, backdrop, stars, overlay,
        uniforms: {
          cameraCenter: uniform(gl, mesh, 'u_cameraCenter'),
          viewport: uniform(gl, mesh, 'u_viewport'),
          cameraZoom: uniform(gl, mesh, 'u_cameraZoom'),
          replayRoot: uniform(gl, mesh, 'u_replayRoot'),
          facing: uniform(gl, mesh, 'u_facing'),
          modelScale: uniform(gl, mesh, 'u_modelScale'),
          depthRange: uniform(gl, mesh, 'u_depthRange'),
          profile: uniform(gl, mesh, 'u_profile'),
          skinned: uniform(gl, mesh, 'u_skinned'),
          texture: uniform(gl, mesh, 'u_texture'),
          boneMatrices: uniform(gl, mesh, 'u_boneMatrices'),
          tint: uniform(gl, mesh, 'u_tint'),
        },
        starPointSize: uniform(gl, stars, 'u_pointSize'),
        overlayUniforms: {
          cameraCenter: uniform(gl, overlay, 'u_cameraCenter'),
          viewport: uniform(gl, overlay, 'u_viewport'),
          cameraZoom: uniform(gl, overlay, 'u_cameraZoom'),
          devicePixelRatio: uniform(gl, overlay, 'u_devicePixelRatio'),
          disc: uniform(gl, overlay, 'u_disc'),
        },
      };
      this.whiteTexture = this.uploadTexture(1, 1, new Uint8Array([255, 255, 255, 255]));
      this.createStars();
      this.createOverlayBuffer();
      gl.clearColor(5 / 255, 7 / 255, 18 / 255, 1);
    } catch (error) {
      if (this.programs) this.destroyGpuResources();
      else {
        if (mesh) gl.deleteProgram(mesh);
        if (backdrop) gl.deleteProgram(backdrop);
        if (stars) gl.deleteProgram(stars);
        if (overlay) gl.deleteProgram(overlay);
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
      this.fighterMeshes = source.fighters.map(fighter => this.uploadMesh(fighter.model, true));
    } catch (error) {
      this.destroyMeshes();
      throw error;
    }
  }

  render(scene: SceneSnapshot): void {
    this.draw(scene.camera);
  }

  draw(camera: CameraState): void {
    if (this.lastCamera) {
      this.lastCamera.mode = camera.mode;
      this.lastCamera.centerX = camera.centerX;
      this.lastCamera.centerY = camera.centerY;
      this.lastCamera.zoom = camera.zoom;
      this.lastCamera.targetPort = camera.targetPort;
      this.lastCamera.smoothing = camera.smoothing;
    } else {
      this.lastCamera = { ...camera };
    }
    if (this.disposed || this.lost || !this.programs) return;
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
    for (let index = 0; index < this.fighterMeshes.length; index++) {
      const mesh = this.fighterMeshes[index];
      const fighter = this.source.fighters[index];
      if (!fighter?.visible) continue;
      // C composites fighters in slot order, so each slot starts a fresh depth pass.
      gl.clear(gl.DEPTH_BUFFER_BIT);
      if (mesh.uploadedPoseVersion !== fighter.poseVersion) {
        this.uploadBoneRows(mesh, fighter.boneRows);
        mesh.uploadedPoseVersion = fighter.poseVersion;
      }
      this.drawMesh(mesh, camera, true, fighter.rootX, fighter.rootY, fighter.facing, 1);
    }
    this.drawOverlays(camera);
    gl.bindVertexArray(null);
  }

  get gpuBytes(): number {
    return this.stageMeshes.reduce((sum, mesh) => sum + mesh.gpuBytes, 0)
      + this.fighterMeshes.reduce((sum, mesh) => sum + mesh.gpuBytes, 0)
      + this.overlayData.byteLength;
  }

  get overlayTruncated(): boolean { return this.overlayTruncatedValue; }

  dispose(): void {
    if (this.disposed) return;
    this.disposed = true;
    this.canvas.removeEventListener('webglcontextlost', this.handleLost);
    this.canvas.removeEventListener('webglcontextrestored', this.handleRestored);
    this.destroyGpuResources();
  }

  private uploadTexture(width: number, height: number, rgba: Uint8Array): WebGLTexture {
    if (width > this.capabilities.maxTextureSize || height > this.capabilities.maxTextureSize) {
      throw new Error(`texture ${width}×${height} exceeds this GPU's ${this.capabilities.maxTextureSize}px limit`);
    }
    const gl = this.gl;
    const texture = gl.createTexture();
    if (!texture) throw new Error('WebGL2 could not allocate a texture');
    gl.bindTexture(gl.TEXTURE_2D, texture);
    gl.pixelStorei(gl.UNPACK_ALIGNMENT, 1);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA8, width, height, 0, gl.RGBA, gl.UNSIGNED_BYTE, rgba);
    return texture;
  }

  private uploadMesh(model: ModelAsset, profile: boolean): GpuMesh {
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
      for (const texture of model.textures) textures.push(this.uploadTexture(texture.width, texture.height, texture.rgba));
      const textureBytes = model.textures.reduce((sum, texture) => sum + texture.rgba.byteLength, 0);
      const boneTexture = profile ? this.createBoneTexture(model.boneCount) : null;
      return {
        vao, buffers, textures, boneTexture, uploadedPoseVersion: -1, model,
        minDepth: profile ? Math.min(-100, bounds[0]) : bounds[2],
        maxDepth: profile ? Math.max(100, bounds[3]) : bounds[5],
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
  ): void {
    if (!this.programs || !this.whiteTexture) return;
    const gl = this.gl;
    const u = this.programs.uniforms;
    gl.useProgram(this.programs.mesh);
    gl.bindVertexArray(mesh.vao);
    gl.uniform2f(u.cameraCenter, camera.centerX, camera.centerY);
    gl.uniform2f(u.viewport, this.size.width, this.size.height);
    gl.uniform1f(u.cameraZoom, camera.zoom);
    gl.uniform2f(u.replayRoot, rootX, rootY);
    gl.uniform1f(u.facing, facing < 0 ? -1 : 1);
    gl.uniform1f(u.modelScale, modelScale);
    gl.uniform2f(u.depthRange, mesh.minDepth, mesh.maxDepth);
    gl.uniform1i(u.profile, profile ? 1 : 0);
    gl.uniform1i(u.skinned, profile ? 1 : 0);
    gl.uniform1i(u.texture, 0);
    gl.uniform1i(u.boneMatrices, 1);
    gl.activeTexture(gl.TEXTURE1);
    gl.bindTexture(gl.TEXTURE_2D, mesh.boneTexture ?? this.whiteTexture);
    for (let index = 0; index < mesh.model.primitiveGroups.length; index++) {
      const group = mesh.model.primitiveGroups[index];
      const phong = index < mesh.model.phongs.length ? mesh.model.phongs[index] : null;
      if (phong) {
        gl.uniform4f(u.tint, phong.diffuse[0] / 255, phong.diffuse[1] / 255, phong.diffuse[2] / 255,
          Math.min(1, Math.max(0, phong.alpha)) * phong.diffuse[3] / 255);
      } else {
        const flags = group.materialFlags;
        gl.uniform4f(u.tint, (150 + ((flags >>> 4) & 0x3f)) / 255,
          (170 + ((flags >>> 12) & 0x3f)) / 255,
          (200 + ((flags >>> 20) & 0x3f)) / 255, 1);
      }
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

  private drawOverlays(camera: CameraState): void {
    if (!this.programs || !this.overlayVao || !this.overlayBuffer) return;
    this.overlayLineVertices = 0;
    this.overlayPointVertices = 0;
    this.overlayTruncatedValue = false;
    const state = this.source.stageState;
    if (Number.isFinite(state.fodLeft))
      this.pushOverlayLine(-58, state.fodLeft, -18, state.fodLeft, 0.35, 0.82, 1, 0.8);
    if (Number.isFinite(state.fodRight))
      this.pushOverlayLine(18, state.fodRight, 58, state.fodRight, 0.35, 0.82, 1, 0.8);
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
      if (item.typeId === 0x36 || item.typeId === 0x37) {
        const red = item.typeId === 0x36 ? 1 : 80 / 255;
        const green = item.typeId === 0x36 ? 76 / 255 : 185 / 255;
        const blue = item.typeId === 0x36 ? 64 / 255 : 1;
        const tail = (item.velocityX >= 0 ? -14 : 14) / camera.zoom;
        this.pushOverlayLine(item.x + tail, item.y, item.x, item.y, red, green, blue, 235 / 255);
        this.pushOverlayLine(item.x + tail / 2, item.y - 1 / camera.zoom, item.x, item.y - 1 / camera.zoom,
          235 / 255, 245 / 255, 1, 180 / 255);
      } else if (item.typeId === 106 || item.typeId === 107) {
        this.pushOverlayPoint(item.x, item.y, 9 / camera.zoom, 0.58, 0.88, 1, 0.88);
      } else {
        this.pushOverlayPoint(item.x, item.y, 8 / camera.zoom, 1, 240 / 255, 170 / 255, 220 / 255);
      }
    }
    for (let index = 0; index < this.source.fighters.length; index++) {
      const fighter = this.source.fighters[index];
      if (fighter.visible && !fighter.follower && fighter.actionState >= 178 && fighter.actionState <= 182 && fighter.shield > 0)
        this.pushOverlayPoint(fighter.rootX, fighter.rootY, fighter.shield * 0.5, 120 / 255, 200 / 255, 1, 85 / 255);
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
    gl.uniform2f(uniforms.cameraCenter, camera.centerX, camera.centerY);
    gl.uniform2f(uniforms.viewport, this.size.width, this.size.height);
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
    for (const mesh of this.fighterMeshes) this.deleteMesh(mesh);
    this.stageMeshes = [];
    this.fighterMeshes = [];
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
    if (this.whiteTexture) gl.deleteTexture(this.whiteTexture);
    if (this.programs) {
      gl.deleteProgram(this.programs.mesh);
      gl.deleteProgram(this.programs.backdrop);
      gl.deleteProgram(this.programs.stars);
      gl.deleteProgram(this.programs.overlay);
    }
    this.abandonGpuResources();
  }

  private abandonGpuResources(): void {
    this.stageMeshes = [];
    this.fighterMeshes = [];
    this.starVao = null;
    this.starBuffer = null;
    this.overlayVao = null;
    this.overlayBuffer = null;
    this.whiteTexture = null;
    this.programs = null;
  }
}
