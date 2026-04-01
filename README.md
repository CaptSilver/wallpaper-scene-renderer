# Wallpaper Engine Scene Renderer

Open source Vulkan scene renderer for Wallpaper Engine wallpapers on Linux.

- Vulkan 1.1
- Render graph for automatic pass dependencies
- HLSL-to-GLSL shader translation
- Async shader compilation with disk cache

## Feature Checklist

### Layers
- [x] Image layers
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
- [x] **Control Points** (up to 8)
  - [x] Mouse follow (`link_mouse`)
  - [x] Worldspace mode
  - [x] Per-instance offset overrides
- [x] Child particle systems (static, eventfollow, eventspawn, eventdeath)
- [x] Instance overrides (color, alpha, brightness, count, lifetime, rate, speed, size)
- [x] Sprite sheet animation (sequential / random frames)
- [x] Perspective rendering flag
- [ ] Collision operators
- [ ] Boids (flocking behavior)
- [ ] Real-time particle lighting
- [ ] Audio-responsive emission (`audioprocessingmode`)

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
  - [ ] `createLayer`, `destroyLayer`
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
- [ ] Media integration events (`mediaPlaybackChanged`, `mediaPropertiesChanged`, etc.)
- [x] `ILocalStorage` (in-memory key-value store: get/set/remove/clear)
- [x] Text layer dynamic content (scripts driving text updates)
- [x] Color scripts (`colorScript` field parsed, compiled, evaluated at 30Hz)
- [x] `WEMath`, `WEColor` utility modules
- [x] `Vec3` factory (position math for cursor/drag scripts)
- [ ] `WEVector` utility module
- [x] Sound layer control (`enumerateLayers`, `play`/`stop`/`pause`/`isPlaying`/`volume`)
- [x] `console.log` support (buffered → LOG_INFO flush)

### Timeline Animations
- [ ] Keyframe system with bezier curve interpolation
- [ ] Animation modes: Loop, Mirror, Single
- [ ] Animatable properties: origin, angles, scale, color, alpha, etc.
- [ ] Auto-keyframing
- [ ] SceneScript animation events

### Not Yet Investigated
- [ ] RGB hardware lighting integration
- [ ] User shortcuts (`engine.openUserShortcut`)
- [ ] `IParticleSystem` / `IParticleSystemInstance` SceneScript control
- [ ] `IMaterial` dynamic shader property access via SceneScript
- [ ] `IVideoTexture` (video as texture source)
- [ ] Layer creation/destruction at runtime via SceneScript
