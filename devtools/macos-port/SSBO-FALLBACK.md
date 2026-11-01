# Porting an SSBO shader to a 4.1 driver

How to make a BAR GL4 shader that reads the engine's per-object uniform buffer
work on a driver without shader storage buffer objects — macOS, where OpenGL is
frozen at 4.1 and SSBOs are a 4.3 feature.

Read this before converting a shader. The engine side is already done; what
remains is per-shader work, and the layout table below is the part you cannot
guess.

## The problem in one paragraph

BAR draws health bars, state icons, selection markers and similar overlays with
one instanced draw call each. The geometry is trivial and carries only a small
integer per instance; the shader then fetches that object's data out of a large
GPU-resident array the engine refreshes every frame:

```glsl
layout(std140, binding=1) readonly buffer UniformsBuffer {
    SUniformsBuffer uni[];
};
#define UNITUNIFORMS uni[instData.y]
```

An unbounded array indexed by a computed integer is exactly what an SSBO is for,
and SSBOs need GL 4.3. Below that the shader does not compile at all, so the
whole feature disappears.

## What the engine now provides

`ModelUniformsUploader` publishes the same bytes as a **buffer texture** when
`globalRendering->haveGL4` is false. A buffer texture is core since GL **3.1**
and gives the same capability: a large array addressed by an integer, read with
`texelFetch`.

| | |
|---|---|
| Lua texture name | `$model_uniforms` |
| GL target | `GL_TEXTURE_BUFFER` |
| Internal format | `GL_RGBA32F` |
| GLSL sampler type | `samplerBuffer` |
| Texels per element | **8** (`sizeof(ModelUniformData)` is 128 bytes) |
| Element capacity | 4096, grown in steps of 2048 |

`gl.TextureInfo("$model_uniforms")` returns `xsize` = current element capacity
and `ysize` = texels per element. **`xsize == 0` means the SSBO path is active**
and the shader should use the SSBO instead — that is the runtime test for which
branch to take, and it is why the engine returns an id of zero rather than
failing.

Only `ModelUniformsUploader` (SSBO binding **1**) has a fallback, and that is
the one these shaders read. `TransformsUploader` (binding **0**, the per-object
matrices) does **not**: it is triple-buffered, and pointing a texture at one
segment of a ring buffer needs `glTexBufferRange`, which is itself 4.3. A shader
that needs object transforms cannot be converted this way.

## The data layout

`ModelUniformData` (`rts/Rendering/Models/ModelsMemStorageDefs.h`) is 128 bytes,
16-byte aligned, so each element is exactly 8 `RGBA32F` texels with no padding
to work around. For element index `i`, the base texel is `i * 8`.

| Texel | `.x` | `.y` | `.z` | `.w` |
|---|---|---|---|---|
| +0 | `composite1` | `composite2` | unused | unused |
| +1 | `maxHealth` | `health` | unused | unused |
| +2 | `drawPos.x` | `drawPos.y` | `drawPos.z` | `drawPos.w` (heading) |
| +3 | `speed.x` | `speed.y` | `speed.z` | `speed.w` |
| +4 | `userDefined[0..3]` | | | |
| +5 | `userDefined[4..7]` | | | |
| +6 | `userDefined[8..11]` | | | |
| +7 | `userDefined[12..15]` | | | |

`composite1` and `composite2` are **bitfields packed into a `uint32`**, and the
texture is float-formatted, so they must be bit-cast rather than read as
numbers:

- `composite1`: `drawFlag` = bits 0-7, `id` = bits 16-31
- `composite2`: `teamID` = bits 0-7

Reading those with `int(texelFetch(...).x)` silently yields nonsense. Use
`floatBitsToUint`.

## The conversion

Add this preamble to the shader, keeping the SSBO declaration intact so the same
source still works on a GL4 driver:

```glsl
#ifdef USE_TEXBUFFER_UNIFORMS
uniform samplerBuffer modelUniforms;

#define MU_TEXELS 8

#define MU_TEXEL(i, t)   texelFetch(modelUniforms, int(i) * MU_TEXELS + (t))

#define MU_DRAWFLAG(i)   (floatBitsToUint(MU_TEXEL(i, 0).x) & 0xFFu)
#define MU_ID(i)         ((floatBitsToUint(MU_TEXEL(i, 0).x) >> 16) & 0xFFFFu)
#define MU_TEAMID(i)     (floatBitsToUint(MU_TEXEL(i, 0).y) & 0xFFu)
#define MU_MAXHEALTH(i)  (MU_TEXEL(i, 1).x)
#define MU_HEALTH(i)     (MU_TEXEL(i, 1).y)
#define MU_DRAWPOS(i)    (MU_TEXEL(i, 2))
#define MU_SPEED(i)      (MU_TEXEL(i, 3))
#define MU_USERDEF(i, k) (MU_TEXEL(i, 4 + ((k) >> 2))[(k) & 3])
#else
layout(std140, binding=1) readonly buffer UniformsBuffer {
    SUniformsBuffer uni[];
};
#endif
```

Then replace each field access. Where the shader says:

```glsl
#define UNITUNIFORMS uni[instData.y]
...
float frac = UNITUNIFORMS.health / UNITUNIFORMS.maxHealth;
vec4  pos  = UNITUNIFORMS.drawPos;
float ud   = UNITUNIFORMS.userDefined[0][2];   // note: vec4[4] in the shader
```

it becomes:

```glsl
float frac = MU_HEALTH(instData.y) / MU_MAXHEALTH(instData.y);
vec4  pos  = MU_DRAWPOS(instData.y);
float ud   = MU_USERDEF(instData.y, 2);        // flat index 0..15
```

Watch the `userDefined` indexing. BAR declares it as `vec4[4]` because std140
pads a `float[16]`, so shader code reads `userDefined[a][b]`. The macro above
takes a **flat** index `0..15`, which is `a * 4 + b`.

On the Lua side, bind the texture and declare the sampler:

```lua
local useTexBuffer = (gl.TextureInfo("$model_uniforms") or {}).xsize
useTexBuffer = (useTexBuffer or 0) > 0

shaderConfig = {
    USE_TEXBUFFER_UNIFORMS = useTexBuffer and 1 or nil,
}
uniformInt = {
    modelUniforms = 5,   -- any free texture unit
}
...
gl.Texture(5, "$model_uniforms")   -- before the draw
...
gl.Texture(5, false)               -- after
```

## Verifying a conversion

Do all three. The first two have both passed for the engine side already; the
third is what tells you your shader is right.

1. **The buffer exists and binds.** Run with `SPRING_DBG_TEXBUF=1` and look for:

   ```
   [ModelUniformsUploader::InitTexBuffer] buffer-texture fallback active: tex=64 vbo=19 elems=4096 (8 texels/elem)
   ```

2. **The data arrives.** The same variable makes the uploader read one element
   back off the GPU and compare it with the CPU-side value it came from:

   ```
   [TEXBUF] elem 2: cpu(id=28331 hp=3700.0/3700.0) gpu(id=28331 hp=3700.0/3700.0) -> MATCH
   ```

   A buffer that exists and a texture that binds say nothing about whether the
   bytes arrived; this is the check that does.

3. **The shader reads the right fields.** Have it output a value you can
   predict — `health / maxHealth` as a colour ramp, say — and compare against a
   unit you damage on purpose. Do not trust "it compiles and draws something":
   a wrong texel offset still compiles and still draws, just with another
   object's data, and at a glance it looks plausible.

## Candidates, in the order worth doing

Thirteen distinct features fail on SSBO. Several share a shader family, so
converting one makes its neighbours cheap. Each has a `(NoGS)` twin whose SSBO
access is identical.

**Readability — the reason this exists:**

- Health Bars (`gui_healthbars_gl4.lua`) — do this first; its layout is already
  understood and it exercises every field type
- Rank Icons, energy / hold-fire / repeat state icons — one family, four widgets
- Selected Units, Ally Selected Units, Unit Groups — selection markers
- Sensor Ranges LOS / Radar / Sonar, Radar Preview — range overlays
- Reclaim Field — reclaimable-material highlighting

**Decoration — do not bother:**

- Map Grass, Airjets, Orb Effects, Ground AO Plates, Resurrection Halos

## Things that will waste your time

- **`GL_SHADER_STORAGE_BUFFER` is not a usable buffer target on a 4.1 driver.**
  It raises `GL_INVALID_ENUM`, being a 4.3 enum. The fallback creates its buffer
  under `GL_TEXTURE_BUFFER` for that reason; do not "tidy" it back.
- **Do not gate the fallback on `glContextIsCore`.** The condition is
  `!haveGL4`, which is what actually determines whether the SSBO path ran.
- **Do not remove the SSBO declaration.** The same shader source runs on real
  GL4 systems, where the SSBO is faster and correct. Both branches stay.
- **`texelFetch` takes an `int`, not a `uint`.** `instData` is `uvec4`, so cast.
  Some drivers accept the mismatch and macOS does not.
- **Size is not a constraint.** `GL_MAX_TEXTURE_BUFFER_SIZE` measured
  268,435,456 texels against the 32,768 this data needs. If you are tempted to
  pack things tighter to save space, don't.
- **A readback of `0` for everything usually means the element index is wrong,**
  not that the buffer is empty — check `elems` in the init line and whether
  `instData.y` is the index you think it is.

## Where the engine side lives

- `rts/Rendering/ModelsDataUploader.{h,cpp}` — `InitTexBuffer`,
  `UpdateTexBuffer`, `KillTexBuffer`, and the `SPRING_DBG_TEXBUF` probes
- `rts/Lua/LuaOpenGLUtils.cpp` — `LUATEX_MODELUNIFORMS` / `$model_uniforms`,
  its `GL_TEXTURE_BUFFER` target, and the size reporting
- `rts/Rendering/Models/ModelsMemStorageDefs.h` — `ModelUniformData`, the
  authority for the layout table above. **If that struct changes, the table and
  the macros are wrong and nothing will warn you.** The `static_assert` on
  `sizeof(ModelUniformData) == 128` is the only tripwire.
