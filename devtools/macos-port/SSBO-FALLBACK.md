# Porting an SSBO shader to a 4.1 driver

How to make a BAR GL4 shader that reads the engine's per-object uniform buffer
work on a driver without shader storage buffer objects — macOS, where OpenGL is
frozen at 4.1 and SSBOs are a 4.3 feature.

Read this before converting a shader. The engine side is already done; what
remains is per-shader work, and the layout table below is the part you cannot
guess.

## STOP: read this before converting anything else

**Converting a shader is not enough to make its feature appear.** Most of these
widgets get their unit list from `api_unit_tracker_gl4.lua`, which declares
`depends = { "gl4" }` and is therefore refused at load wherever GL4 is missing.
Its consumers then have no units to draw and submit zero instances, no matter
how correct their shader is:

```
Missing capabilities:  API Unit Tracker DEVMODE GL4. Disabling.
Unit Tracker API unavailable, exiting Orb GL4
Jammed Units Highlight: Unit Tracker API unavailable, exiting
```

That gate is **wrong**, not a real requirement. The widget's job is maintaining
`WG.unittrackerapi.visibleUnits` / `alliedUnits`, which is pure Lua; every GL4
use in the file, `initGL4()` included, sits behind `if debugdrawvisible then`,
and that defaults to `false`. One unnecessary dependency line disables an API
that health bars, orb effects, jammed units, sensor ranges, energy icons,
firestate icons, idle-builder icons, ground AO plates, defense range and the
outline shader all read.

**It cannot be fixed from the write dir.** Dropping a corrected copy in
`<write-dir>/LuaUI/Widgets/` loads, but BAR sandboxes `widgetHandler` for user
widgets and the tracker needs to call `widgetHandler:VisibleUnitsChanged(...)`
to fan out to consumers:

```
Warning: API Unit Tracker DEVMODE GL4: widgetHandler.VisibleUnitsChanged is not available to user widgets
```

Only an archive ("mod") widget may register custom callins. So the options are:

1. **Report it upstream to BAR** — the honest fix, a one-line change to that
   file, and it helps every GL4-less machine, not just macOS.
2. **Ship a mutator `.sdd`** that depends on the BAR archive and shadows the
   file, so the override counts as an archive widget. Works, but a mutator has
   its own gametype string, so games started from the Chobby menu would not use
   it — only scripted launches would.

Until one of those happens, a converted shader can be verified as *compiling*
but not as *drawing*. Convert against that expectation, and do not read an empty
screen as a broken conversion.

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

**Two** buffer textures, both bound by the engine on reserved units, so a shader
names its unit with `layout(binding = N)` and **no widget has to bind anything**.
`BuildGlslCompatPreamble` lifts that binding out of the source and re-applies it
with `glUniform1i` after linking, which is how the declaration survives on a
driver without 4.2.

| SSBO binding | Lua name | unit | element | texels | GLSL |
|---|---|---|---|---|---|
| 1 (`ModelUniformsUploader`) | `$model_uniforms` | **14** | `ModelUniformData`, 128 B | 8 | `samplerBuffer` |
| 0 (`TransformsUploader`) | `$model_transforms` | **13** | `Transform`, 32 B | 2 | `samplerBuffer` |

`Transform` is `{ vec4 quat; vec4 trSc; }` - texel 0 is the quaternion, texel 1
is translation in `.xyz` and scale in `.w`. It maps 1:1 onto the engine's
`CQuaternion r; float3 t; float s;`.

An earlier version of this file said the transforms could not be done this way,
because that SSBO is triple-buffered and aiming a texture at one segment of a
ring needs `glTexBufferRange` (4.3). That was wrong: the fallback makes its
**own single buffer** from the same CPU-side storage, so the ring never enters
into it.

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

**Rebuild the struct, do not rewrite the body.** BAR's shaders declare their own
`SUniformsBuffer` mirroring the engine's layout, so the cheapest conversion
replaces the `buffer` block with a function that fills that same struct from
texels, and redefines the accessor macro to call it. Every `UNITUNIFORMS.field`
below it then works untouched:

```glsl
uniform samplerBuffer modelUniforms;

SUniformsBuffer fetchUni(uint idx) {
    int b = int(idx) * 8;            // 128 bytes == 8 texels

    vec4 t0 = texelFetch(modelUniforms, b + 0);
    vec4 t1 = texelFetch(modelUniforms, b + 1);

    SUniformsBuffer u;
    // t0 holds packed integers and the texture is float-formatted
    u.composite = floatBitsToUint(t0.x);
    u.unused2   = floatBitsToUint(t0.y);
    u.unused3   = floatBitsToUint(t0.z);
    u.unused4   = floatBitsToUint(t0.w);

    u.maxHealth = t1.x;
    u.health    = t1.y;
    u.unused5   = t1.z;
    u.unused6   = t1.w;

    u.drawPos = texelFetch(modelUniforms, b + 2);
    u.speed   = texelFetch(modelUniforms, b + 3);

    u.userDefined[0] = texelFetch(modelUniforms, b + 4);
    u.userDefined[1] = texelFetch(modelUniforms, b + 5);
    u.userDefined[2] = texelFetch(modelUniforms, b + 6);
    u.userDefined[3] = texelFetch(modelUniforms, b + 7);

    return u;
}
```

then `#define UNITUNIFORMS fetchUni(instData.y)` in place of
`uni[instData.y]`. Match the field names to whatever the shader's own struct
declares - they vary slightly between shaders.

This fetches all eight texels even when the shader reads two, which has not
mattered in practice. If a profile ever says otherwise, the per-field macros
below read only what they need.

### Per-field macros, if you need them

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

## Worked example: health bars (done)

`devtools/macos-port/bar-overrides/` holds the two patches, against the files as
the BAR archive ships them. The measured cost of one feature:

| | |
|---|---|
| Shader diff | ~40 lines, almost all the `fetchUni` function |
| Widget diff | 3 hunks, 5 lines |
| Shader body changes | **none** |
| Result | compiles in 10 ms; failing programs 29 -> 28 |

Only the **NoGS** variant needed converting. The geometry-shader variant still
fails, and BAR falls back to NoGS on its own - so for a feature with a NoGS
twin, convert that one and leave the other alone.

### Deploying an override without touching the archive

BAR's content is in a read-only rapid archive, but it does not need patching in
place:

- `<write-dir>/LuaUI/Widgets/` is scanned with `vfsRaw`, so a widget placed
  there loads instead of the archive's (the log says `Loading widget from
  user:` rather than `from mod:`).
- For unsynced Lua, `VFS.LoadFile`'s default mode is `RAW_FIRST`, so a shader
  file at `<write-dir>/LuaUI/Shaders/<SameName>.glsl` **shadows** the archive
  copy with no widget change at all. Keep the filename and capitalisation
  exactly as the widget's `vssrcpath` spells it.

So: extract the originals from the archive, apply the patch, drop the results in
the write dir. Nothing in the archive is modified, and removing the files
restores stock behaviour.

Note that the write dir is gitignored and BAR rewrites parts of it, so these
patches are the durable copy - not the deployed files.

## The big multiplier: convert the shared code, not the shaders

Most of BAR's per-unit overlays do not have their own shader. They call
`InitDrawPrimitiveAtUnit` from `LuaUI/Include/DrawPrimitiveAtUnit.lua`, which
builds one shader for all of them - **24 widgets share it**. And the transform
accessors live in one more shared place, `modules/graphics/luashader.lua`'s
`GetQuaternionDefs()`, which every quaternion-transform shader embeds.

So two file conversions covered eleven shader programs at once:

| File | What it carries |
|---|---|
| `modules/graphics/luashader.lua` | the `TransformBuffer` SSBO + four accessors |
| `LuaUI/Shaders/DrawPrimitiveAtUnit_nogs.vert.glsl` | two reads of `uni[].composite` |

Result: rank / energy / hold-fire / repeat icons, selected units (plus ally and
unfinished variants), unit groups, resurrection halos and ground AO plates all
compile. Failing programs went 29 -> 20.

**Before converting any individual shader, check whether its SSBO access comes
from shared code.** `grep -rl DrawPrimitiveAtUnit` and
`grep -rn "transforms\[" ` are the two searches worth doing first.

In `GetQuaternionDefs` every read went through `GetModelWorldTransform`,
`GetPieceModelTransform`, `GetPieceWorldTransform` and
`GetStaticPieceModelTransform`, so swapping `transforms[i]` for a
`fetchTransform(i)` function left all four working and no caller changed.

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

## The override archive: how the BAR patches are deployed

The blocker above is fixed locally by a **`.sdd` override archive** rather than
write-dir files, because only an archive widget escapes BAR's user-widget
sandbox. `devtools/macos-port/bar-overrides/` holds the source of truth:

```
modinfo.lua                  the archive manifest, copied verbatim
unit-tracker-ungate.patch    drops depends = {"gl4"} from the tracker API
healthbars-widget.patch      binds $model_uniforms, declares the sampler
healthbars-nogs-vert.patch   the SSBO -> buffer-texture conversion
```

Deploy by building `<write-dir>/games/bar-macos.sdd/`:

```
games/bar-macos.sdd/
  modinfo.lua
  LuaUI/Widgets/api_unit_tracker_gl4.lua
  LuaUI/Widgets/gui_healthbars_gl4.lua
  LuaUI/Shaders/HealthbarsGL4_nogs.vert.glsl
```

Extract each original from the BAR archive, apply its patch, and place it at the
same path. `modinfo.lua` declares `depend` on the stock BAR archive, so BAR is
still loaded underneath and only these files are shadowed.

Three things that cost time getting this working:

- **The gametype string is `name .. " " .. version`,** not `name`. This archive
  registers as **`BAR macOS overrides 1`**; a start script saying
  `gametype=BAR macOS overrides` fails with `Dependent archive ... not found`,
  which reads as if the archive were missing rather than misnamed. Check
  `cache/ArchiveCache22.lua` for the registered `name` when in doubt.
- **`depend` must name the BAR archive exactly**, version included
  (`Beyond All Reason test-31765-420384b`). It changes whenever BAR updates, so
  this is the line that will need maintaining.
- **The archive cache can hide a modinfo mistake.** `cache/ArchiveCache22.lua`
  is keyed on mtime; if a scan looks like it did nothing, move that file aside
  and re-run.

### Known limitation

A game started from the **Chobby menu** uses BAR's own gametype and therefore
**does not load this archive**. Only launches that name the gametype explicitly
(a start script) get the overrides. So menu play still needs the upstream BAR
fix for the tracker gate - the archive is for testing and for scripted games.

### Verified

With the archive in use: no `Missing capabilities: API Unit Tracker` line, no
`Unit Tracker API unavailable` from any consumer, the converted NoGS shader
compiles in 10 ms, and the health-bar widget submits real work -
`HBPROBE, 12, 0`, twelve instances - where it submitted zero before.
