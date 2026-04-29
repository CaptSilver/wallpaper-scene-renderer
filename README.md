# Wallpaper Engine Scene Renderer

Open source Vulkan scene renderer for Wallpaper Engine wallpapers on Linux.

- Vulkan 1.1
- Render graph for automatic pass dependencies
- HLSL-to-GLSL shader translation
- Async shader compilation with disk cache

## Feature Checklist

### Layers
- [x] Image layers
  - [x] `autosize: true` in model JSON — size resolved from the first texture's sprite frame dimensions (or `mapWidth/mapHeight` for non-sprite pictures)
  - [x] Script-referenced layers stay in the main render graph even when initially invisible (so SceneScript visibility toggles actually render — e.g. dino_run's jump sprite)
  - [x] Dynamic-asset pool for `thisScene.createLayer(asset)` — pre-allocated pool of hidden scene nodes per `engine.registerAsset(path)` (both image and particle assets).  Burst particle FX rearm via `ParticleSubSystem::Reset` on each pool-pop and auto-hide once all particles die (`IsBurstDone`), so pool slots release even when scripts forget to call `destroyLayer`.
- [x] Composition / Fullscreen layers
- [x] Text (static rasterization via FreeType)
- [x] Text (dynamic via SceneScript — clocks, dates, counters)
- [x] Sound objects (looping playback via miniaudio)
- [x] Particle systems (see below)
- [x] 3D models (see below)
- [x] Light objects (point lights, skylight)

### Effects (generic shader pipeline)
Built-in effects work via the same shader pipeline — any effect that compiles
to a valid fragment shader will render. Known working:

- [x] **Animation**: Scroll, Spin, Shake, Pulse, Water Flow, Water Ripple, Water Waves, Foliage Sway, Swing, Twirl, Cloud Motion
- [x] **Blur**: Blur, Blur Precise, Motion Blur, Radial Blur
- [x] **Interactive**: Cursor Ripple, Depth Parallax, X-Ray
- [x] **Color**: Blend, Blend Gradient, Chromatic Aberration, Color Key, Film Grain, Shimmer, Glitter, Tint, Opacity, VHS, Fire, Light Shafts, Nitro, Reflection, Water Caustics
- [x] **Distortion**: Fisheye, Perspective, Refraction, Skew, Transform
- [x] **Enhancement**: Edge Detection, God Rays, Local Contrast, Shine
- [ ] **Simulation**: Advanced Fluid Simulation
- [ ] **Other**: Iris Movement (needs eye tracking data), Clouds (procedural generation)
- [x] ColorBlendMode (effectpassthrough for complex blend modes)
- [x] Mouse position with delay (`g_PointerPosition`)
- [x] Parallax (mouse-reactive layer offset)
- [x] PBR lighting
- [x] Global bloom
- [x] Ping-pong FBO pipeline
- [x] Per-effect visibility control
- [x] HDR-capable effect rendering

### Camera
- [x] Orthographic (2D scenes)
- [x] Perspective (3D scenes)
- [x] Zoom
- [x] Shake (sum-of-sinusoids)
- [x] Path animation (keyframe interpolation)
- [x] Parallax (mouse-reactive camera offset)
- [x] Fade (transitions between camera paths)

### Audio
- [x] BGM playback (loop)
- [x] Spectrum analysis (KissFFT — 16/32/64 bands, L+R channels)
- [x] Shader uniforms (`g_AudioSpectrum{16,32,64}{Left,Right}`, std140 padded)
- [x] System audio capture (PipeWire/PulseAudio monitor source)
- [x] Playback tap fallback (when system capture unavailable)
- [x] Runtime toggle between system capture and playback tap
- [x] Random sound playback (random track selection + inter-track delay)

### Particle System
- [x] **Renderers**: Sprite, Rope, Sprite Trail, Rope Trail
- [x] **Geometry shader renderers**: GS Sprite, GS Rope, GS Sprite Trail
- [x] **Emitters**: Box, Sphere
  - [x] Rate-based emission
  - [x] Burst / instantaneous emission
  - [x] One-per-frame mode
  - [x] Periodic emission (delay, duration, max-per-period)
  - [x] Duration (emitter lifetime limit)
- [x] **Initializers**:
  - [x] `colorrandom` — random color (min/max RGB)
  - [x] `hsvcolorrandom` — random color in HSV space
  - [x] `colorlist` — pick from discrete color palette
  - [x] `lifetimerandom` — random lifetime (min/max, exponent)
  - [x] `sizerandom` — random size (min/max, exponent)
  - [x] `alpharandom` — random alpha
  - [x] `velocityrandom` — random velocity (per-axis min/max)
  - [x] `rotationrandom` — random rotation
  - [x] `angularvelocityrandom` — random angular velocity
  - [x] `turbulentvelocityrandom` — curl noise velocity
  - [x] `positionoffsetrandom` — perpendicular offset (rope particles)
  - [x] `box` — box position initializer
  - [x] `sphere` — sphere position initializer
  - [x] `remapinitialvalue` — map input values to output properties
  - [x] `mapsequencebetweencontrolpoints` — distribute along line between CPs (arc, mirror, noise)
  - [x] `mapsequencearoundcontrolpoint` — distribute around a control point
  - [x] `inheritcontrolpointvelocity` — adopt CP velocity at spawn
  - [x] `inheritinitialvaluefromevent` — copy a parent-particle property at spawn (eventspawn / eventdeath children)
- [x] **Operators**:
  - [x] `movement` — velocity + gravity + drag
  - [x] `angularmovement` — angular velocity + force + drag
  - [x] `sizechange` — size over lifetime
  - [x] `alphafade` — alpha fade in/out
  - [x] `alphachange` — alpha over lifetime
  - [x] `colorchange` — color over lifetime
  - [x] `oscillatealpha` — oscillating alpha
  - [x] `oscillatesize` — oscillating size
  - [x] `oscillateposition` — oscillating position
  - [x] `turbulence` — curl noise turbulence (blend in/out)
  - [x] `vortex` — vortex force around control point
  - [x] `vortex_v2` — advanced vortex (ring radius, rotation mode)
  - [x] `controlpointattract` — attraction toward control point
  - [x] `maintaindistancebetweencontrolpoints` — distance constraint
  - [x] `reducemovementnearcontrolpoint` — movement dampening near CP
  - [x] `remapvalue` — remap particle property values
  - [x] `controlpointforce` — radial force from control point
  - [x] `capvelocity` — clamp speed to maxvelocity
  - [x] `maintaindistancetocontrolpoint` — soft distance constraint to a single CP
  - [x] `inheritvaluefromevent` — copy event/parent particle property each frame
  - [x] **Collision** — `collisionplane`, `collisionsphere`, `collisionbox` / `collisionbounds`, `collisionquad`, `collisionmodel` (with restitution)
  - [x] `boids` — Reynolds 1987 flocking (separation, alignment, cohesion, maxspeed)
- [x] **Control Points** (up to 8)
  - [x] Mouse follow (`link_mouse`)
  - [x] Worldspace mode
  - [x] Per-instance offset overrides
- [x] Child particle systems (static, eventfollow, eventspawn, eventdeath)
- [x] Instance overrides (color, alpha, brightness, count, lifetime, rate, speed, size)
- [x] Sprite sheet animation (sequential / random frames)
- [x] Perspective rendering flag
- [ ] Real-time particle lighting (sample SceneLights in particle vertex/fragment shader)
- [ ] Audio-responsive emission (`audioprocessingmode` is parsed but not yet wired into emit-rate/size/color)

### Puppet Warp
- [x] Multi-bone skeletal animation
- [x] Play modes: Loop, Mirror, Single
- [x] Animation blending (blend weights)
- [x] Bone frame interpolation
- [x] Animation layer system (visibility, rate)
- [x] Per-vertex skinning (blend indices + weights)
- [ ] Blend shapes / shape animations
- [ ] Inverse kinematics
- [ ] Texture channel animations
- [ ] Interactive puppet warps (SceneScript-triggered)
- [ ] 3D perspective extrusion

### 3D Models
- [x] MDL format parsing (v13+, multi-submesh)
- [x] Vertex attributes: position, normal, tangent, texcoord, texcoord1, blend indices/weights
- [x] Per-submesh materials
- [x] Camera path animation
- [x] Depth occlusion & transparency
- [x] Planar reflection (Y=0 mirror)
- [x] Normal mapping
- [x] Lightmap UVs (secondary UV channel)
- [x] Point lights & skylight
- [x] Skybox (spherical environment mapping)
- [x] MSAA x4
- [x] Anisotropic filtering (16x)
- [x] HDR rendering (RGBA16F internal RTs)
- [x] HDR texture formats (BC6H, RGBA16F, RG16F, R16F, RGB565, RGBA1010102)
- [ ] Shadow mapping
- [ ] HDR bloom post-process
- [ ] Spot lights
- [ ] Volumetric lighting

### Blend Modes
- [x] Translucent (standard alpha)
- [x] Translucent_PA (premultiplied alpha)
- [x] Additive
- [x] Opaque (screen blend for passthrough compose)
- [x] Normal
- [x] Disable (no blending)
- [x] Complex colorBlendMode via effectpassthrough (BlendColor, etc.)

### Texture Formats
- [x] BC1 (DXT1), BC2 (DXT3), BC3 (DXT5), BC7
- [x] BC6H (HDR compressed)
- [x] RGBA8, RGB8, RG8, R8
- [x] RGBA16F, RG16F, R16F
- [x] RGB565, RGBA1010102
- [x] TEXB container (v1–v4)
- [x] FreeImage formats (PNG, JPEG, TGA, BMP, GIF, DDS, PSD, HDR, EXR, etc.)

### User Properties
- [x] Default values from `project.json`
- [x] Runtime visibility bindings (conditional show/hide)
- [x] Runtime uniform bindings (shader value overrides)
- [x] Per-wallpaper config persistence (JSON)
- [x] Combo, slider, color, bool, text property types
- [ ] User-defined custom shaders

### Shader Pipeline
- [x] HLSL-to-GLSL translation (XSC-based)
- [x] GLSL-to-SPIR-V compilation (glslang)
- [x] Geometry shader support (VS+GS+FS linked compilation)
- [x] Async compilation with disk caching
- [x] `#define` injection (`log10`, etc.)
- [x] Implicit conversion fixes (`bool()` wraps, cross-stage varying upgrades)
- [x] Effect alpha preservation (component-write shaders that omit `.a`)
- [x] `flat` interpolation qualifier support
- [x] 13 texture slots (`g_Texture0`–`g_Texture12`) with resolution/rotation/translation metadata

### SceneScript
- [x] `engine` global (IEngine)
  - [x] `registerAudioBuffers(resolution)` → `AudioBuffers { left, right, average }`
  - [x] `screenResolution`, `canvasSize`, `timeOfDay`, `frametime`, `runtime`
  - [x] `userProperties` access
  - [x] `setTimeout` / `setInterval`
  - [x] Device detection (`isDesktopDevice`, `isMobileDevice`, etc.)
- [x] `thisScene` global (IScene)
  - [x] Scene property control (bloom, clear color, camera, lighting)
    - [ ] `bloomEnabled` runtime toggle (requires render graph recompilation)
  - [x] `getLayer(name)` — returns layer proxy with position/scale/visibility/opacity
  - [x] `enumerateLayers()` — returns array of all layer proxies
  - [x] `createLayer(asset)` / `destroyLayer(layer)` — pool-backed for image and particle assets (burst particle FX rearm via `ParticleSubSystem::Reset`; auto-hide on `IsBurstDone`)
  - [x] Camera transforms
- [x] `thisLayer` global (ILayer)
  - [x] Position, angles, scale, visibility control
  - [ ] Parent/child hierarchy manipulation
  - [x] Animation layer access (`getAnimationLayer`, `getAnimationLayerCount`)
- [x] `input` global (IInput)
  - [x] Mouse cursor position and events
  - [x] `cursorEnter` / `cursorLeave` / `cursorClick` events
  - [x] `cursorDown` / `cursorUp` / `cursorMove` events
- [x] `shared` global (inter-script data sharing)
- [x] Events: `init`, `update`
- [x] Events: `destroy`, `applyUserProperties`, `resizeScreen`
- [x] Media integration events (MPRIS D-Bus → `mediaPlaybackChanged`, `mediaPropertiesChanged`, `mediaThumbnailChanged`, `mediaTimelineChanged`, `mediaStatusChanged`)
- [x] `ILocalStorage` (in-memory key-value store: get/set/remove/clear)
- [x] Text layer dynamic content (scripts driving text updates)
- [x] Color scripts (`colorScript` field parsed, compiled, evaluated at 30Hz)
- [x] `WEMath`, `WEColor` utility modules
- [x] `Vec2`, `Vec3`, `Vec4` factories (full method sets — add/sub/mul/div, length/normalize/dot/cross, lerp, distance, fromString, r/g/b aliases)
- [x] `Mat3`, `Mat4` matrix classes (translation/rotation/scale; view + projection helpers for cursor / drag scripts)
- [x] `WEVector` utility module (`angleVector2`, `vectorAngle2`)
- [x] `engine.colorScheme` — `Vec3` from the `schemecolor` user property (refreshed on apply)
- [x] `engine.openUserShortcut(name)` — dispatched to MPRIS (play/pause/next/prev) and to in-scene `scene.on('userShortcut', …)` listeners
- [x] `getEffect(name).visible` runtime per-effect toggle (dirty-tracked)
- [x] `scene.on(event, fn)` / `scene.off(event[, fn])` — in-scene event bus, fans out 10 fixed event points (update, cursor*, media*, resize, …) plus `userShortcut`
- [x] `IVideoTexture` — `thisLayer.getVideoTexture()` returns a proxy with `getCurrentTime`, `setCurrentTime`, `duration`, `rate`, `play`, `pause`, `stop`, `isPlaying` (mpv-backed video texture decoder)
- [x] `IMaterial` — `thisLayer.getMaterial().setValue(name, value)` / `getValue(name)`; uniform writes go through a render-thread queue into `customShader.constValues`, dirty flag re-uploads on next frame.  Reads are JS-side cached so they never race the renderer.
- [x] Sound layer control (`enumerateLayers`, `play`/`stop`/`pause`/`isPlaying`/`volume`)
- [x] `console.log` support (buffered → LOG_INFO flush)

### Timeline Animations
- [ ] Keyframe system with bezier curve interpolation
- [ ] Animation modes: Loop, Mirror, Single
- [ ] Animatable properties: origin, angles, scale, color, alpha, etc.
- [ ] Auto-keyframing
- [ ] SceneScript animation events

### Not Yet Investigated
- [ ] RGB hardware lighting integration (OpenRGB / liquidctl)
- [ ] `IParticleSystem` / `IParticleSystemInstance` SceneScript control

---

## Roadmap — Unimplemented Features

What it would take to finish each unchecked item above. Items are roughly
ordered by scope (smallest → largest).

### Easy / mostly plumbing

- **`bloomEnabled` runtime toggle** — bloom passes are baked into the render
  graph at scene load. Cheapest implementation is a "bypass mode" branch in
  the combine pass; the harder/cleaner path is a graph rebuild on toggle.
- **Audio-responsive particle emission** — `audioprocessingmode` is already
  parsed (`WPParticleObject.cpp:78`) but never consumed. Wire it into the
  emit-rate / size / color paths using the existing `g_AudioSpectrum*`
  uniforms; gate the per-particle sample on the mode value.
- **Parent / child layer hierarchy from SceneScript** — add
  `setParent(layer)` / `getParent()` / `getChildren()` to the layer JS proxy
  and a graph-rebuild hook in `RenderHandler` so reparenting takes effect on
  the next frame.
- **`IParticleSystem` / `IParticleSystemInstance`** — expose a stable
  `nodeId → ParticleSubSystem` map through `__sceneBridge`, then thread
  emission rate / pause / per-instance CP overrides through the existing
  `ParticleModify` queue.
- **User-defined `customshader` user-properties** — accept a `customshader`
  property type, swap the layer's shader pass at runtime through the async
  compile cache. Most plumbing already exists; missing piece is the
  user-property → pass swap.
- **RGB hardware lighting** — pure plumbing: forward
  audio-spectrum + dominant-color outputs to OpenRGB / liquidctl over
  D-Bus. No renderer work.

### Medium

- **Real-time particle lighting** — particles use a separate sprite/rope
  shader path that ignores `SceneLight`s. Add a per-particle sample of
  nearby point lights (vertex or fragment), broadcast positions via a small
  uniform array, and reuse the PBR shader prelude.
- **Procedural Clouds effect** — multi-octave 3D noise (or a 3D noise LUT)
  + a custom-binding for cloud uniforms (cover, density, speed). One new
  shader stage.
- **Spot lights** — extend `SceneLight.hpp` with cone angle, inner/outer
  falloff, and direction; add cone culling in shaders; integrate with the
  PBR prelude alongside point lights and skylight.
- **HDR bloom post-process** — the `combine_hdr` postprocessing variant
  exists (see `blend-tonemap-audit.md` A2), but the per-mip bloom
  convolution chain that feeds it isn't built. Add the down/up sample
  pyramid and feed mips into the combine pass.
- **Puppet: blend shapes** — linear morph-target blending. Adds vertex
  streams for shape deltas and per-shape weight uniforms.
- **Puppet: 3D perspective extrusion** — synthesize Z + normals on a 2D
  puppet so it picks up lighting and parallax. Mostly mechanical.
- **Puppet: texture channel animations** — per-bone texcoord offsets,
  separate from skin matrices. Needs an extra channel parsed out of the
  Puppet binary.
- **Interactive puppet warps (SceneScript-triggered)** — expose Puppet
  bone manipulation through a `thisLayer.getPuppet()` JS proxy with
  `bones[i]` setters and an entry point for triggering authored animations.

### Large

- **Shadow mapping** — depth-only render pass per light, cascade for the
  skylight, PCF/PCSS sampler, integration into the PBR prelude (which
  already takes a `shadowFactor` parameter wired to `1.0` —
  `WPShaderParser.cpp:391`). New light view-proj matrices, depth-target
  attachments, fragment-side sampling.
- **Volumetric lighting** — ray-march from camera through participating
  media; needs depth-buffer access, per-pixel scattering integration, and
  per-light volume bounds. Pairs naturally with shadow mapping.
- **Iris Movement effect** — depends on external eye/face tracking input
  (webcam + tracker daemon). IPC bridge + driver + mapping to layer
  transform. Out of scope until that stack exists.
- **Advanced Fluid Simulation effect** — the renderer has no compute
  pipeline today. Would need `VkComputePipeline` plumbing, pressure /
  velocity field textures, and a Navier-Stokes solver (advect → divergence
  → jacobi → project) split across passes.
- **Puppet: inverse kinematics** — FABRIK/CCD per chain. Limited value
  until interactive puppet warps land.
- **Timeline animations (entire section)** — WE's authoring-time keyframe
  system. Requires:
  - extracting the timeline JSON in `WPSceneParser` (not currently parsed),
  - per-property bezier/Hermite interpolation,
  - loop / mirror / single state machines,
  - binding into the existing dirty-update queues for `SceneNode` /
    `SceneMaterial`.

  Largest single missing system — comparable in scope to SceneScript
  itself. Many wallpapers use SceneScript instead; the gap mostly affects
  authored-without-script scenes.
