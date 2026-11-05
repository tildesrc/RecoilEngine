# Porting an SSBO shader to a 4.1 driver

How to make a BAR GL4 shader that reads the engine's per-object uniform buffer
work on a driver without shader storage buffer objects — macOS, where OpenGL is
frozen at 4.1 and SSBOs are a 4.3 feature.

Read this before converting a shader. The engine side is already done; what
remains is per-shader work, and the layout table below is the part you cannot
guess.

## STOP: read this before converting anything else

**This work is finished.** Every BAR shader that can be converted at 4.1 has
been. If you are here to convert a new one — a BAR update, or a shader nobody
reached — read "The conversion" and "Things that will waste your time", and
skip the rest. The sections below record what the conversions cost and why the
remaining failures are not bugs.

### What still fails, and why that is correct

| Program | Why |
|---|---|
| `radarPreviewCoverage GL4` | its `buffer RayData` is **written** by the shader. A buffer texture is read-only, so there is nothing to convert to; it needs image load/store, which is GL 4.2. |
| the GS variants of the icon / health-bar / selection family | they need a geometry shader BAR only builds for GL4. Each has a `(NoGS)` twin that compiles, and BAR falls back to it unprompted. **These are not failures to chase.** |

### The gate that used to block everything (fixed)

Most of these widgets get their unit list from `api_unit_tracker_gl4.lua`, which
declared `depends = { "gl4" }` and was therefore refused at load wherever GL4 is
missing. Its consumers then had no units to draw and submitted zero instances,
no matter how correct their shader was:

```
Missing capabilities:  API Unit Tracker DEVMODE GL4. Disabling.
Unit Tracker API unavailable, exiting Orb GL4
```

That gate was **wrong**, not a real requirement: the widget's job is maintaining
`WG.unittrackerapi.visibleUnits` / `alliedUnits`, which is pure Lua, and every
GL4 use in the file sits behind `if debugdrawvisible then`, default `false`. One
unnecessary dependency line disabled an API that health bars, orb effects,
jammed units, sensor ranges, energy icons, firestate icons, idle-builder icons,
ground AO plates, defense range and the outline shader all read.

It **cannot** be fixed from the write dir: BAR sandboxes `widgetHandler` for
user widgets, and the tracker has to call `widgetHandler:VisibleUnitsChanged(...)`
to fan out to consumers.

```
Warning: API Unit Tracker DEVMODE GL4: widgetHandler.VisibleUnitsChanged is not
available to user widgets
```

Only an archive ("mod") widget may register custom callins — which is the whole
reason the override `.sdd` exists. See "The override archive" below. Reporting
the one-line fix upstream to BAR is still the honest end state, and would help
every GL4-less machine rather than just this checkout.

The lesson worth keeping: **a shader that compiles is not a feature that draws.**
Verify both, separately.

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

## What was converted

Every conversion below is deployed from `devtools/macos-port/bar-overrides/`.
Failing shader programs went **29 -> 12**, and of those 12, eleven are GS
variants with working NoGS twins and one is `radarPreviewCoverage` (see above).

| Feature | Route |
|---|---|
| Health Bars | `HealthbarsGL4_nogs.vert.glsl` — done first; exercises every field type |
| Rank / energy / hold-fire / repeat icons, selected + ally-selected + unfinished units, unit groups, resurrection halos, ground AO plates | **one** file: `DrawPrimitiveAtUnit_nogs.vert.glsl` plus `luashader.lua`. See "The big multiplier" below. |
| Sensor Ranges LOS / Radar / Sonar | one shared shader pair, `sensor_ranges_los.{vert,frag}.glsl`; radar and sonar additionally own a circle-list buffer |
| Reclaim Field | `reclaim_field_gl4.vert.glsl` + `reclaim_field_renderer_gl4.lua`; widget-owned `ClusterParams` |
| Map Grass | `map_grass_gl4.vert.glsl` + widget; widget-owned `UnitBendBuffer` |
| Airjets, Orb Effects, Paralyze Effect | inline shader strings inside the widget `.lua` |

Decoration (grass, jets, orbs) was deprioritised at the time and converted last,
once it was the only thing left. Worth knowing: it was not slower than the rest.

## Widget-owned buffers: `VBO:BindTexture`

The engine's own per-object data arrives on reserved units (14 and 13). A widget
that creates its **own** SSBO needs a buffer texture of its own, so the Lua VBO
API gained two calls (`rts/Lua/LuaVBOImpl.cpp`, commit `a68cae7c4b`):

```lua
-- GL_SHADER_STORAGE_BUFFER is not even a valid buffer TARGET below GL 4.3, so
-- this has to change at creation, not just at bind time
local buf = gl.GetVBO(GL.TEXTURE_BUFFER, false)
buf:Define(count, { { id = 0, name = "whatever", size = 4 } })
buf:Upload(data)

buf:BindTexture(10)          -- instead of buf:BindBufferRange(binding)
--  ... draw ...
buf:UnbindTexture(10)
```

`BindTexture` re-attaches the buffer to its texture **every call**, on purpose:
growing a VBO reallocates its storage, and a texture attached to the old
allocation reads as empty with no error.

Element layout is unchanged from the SSBO case — `GL_TEXTURE_BUFFER` goes
through the same std140 path, so `size = 4` is one `vec4` is one `RGBA32F` texel
and existing `Define`/`Upload`/`CopyTo` arithmetic carries over untouched.

### Texture units in use

Pick an unused one and keep this table current.

| Unit | Buffer |
|---|---|
| 14 | `$model_uniforms` (engine) |
| 13 | `$model_transforms` (engine) |
| 12 | sensor-range circles (`gui_sensor_ranges_radar/sonar.lua`) |
| 11 | reclaim field `ClusterParams` |
| 10 | grass `UnitBendBuffer` |

## Two traps that are not about SSBOs at all

Both of these were hiding *behind* an SSBO error and only surfaced once the
conversion worked. Both are BAR bugs that every other desktop driver tolerates
and Apple's frontend does not, so expect more of them.

- **`layout(binding = N)` must be a literal.** The engine lifts the binding out
  of the source textually, in `RewriteGlslLayoutBindings`, which runs *before*
  the preprocessor. A `shaderConfig` define is still an identifier at that point
  and the line is skipped, leaving 4.2 syntax the driver rejects. Hardcode the
  number and keep the Lua-side constant in step with a comment.
- **An undefined macro in `#if` is an error in GLSL**, unlike C. BAR's grass
  shader tests `#if DEBUG == 1` and never defines `DEBUG`; that alone cost the
  map its grass. The engine now papers over this for every shader — see
  `GlslZeroUndefinedConditionMacros` — by emitting `#define NAME 0` for any
  identifier used in a `#if`/`#elif` that is never defined, which is what the
  lenient drivers assume anyway. It logs each name once.
- **A `const` initializer must be a constant expression.** The orb shader had
  `const float strength = 0.06 + 0.3 * violence;`. Dropping `const` is the fix;
  there is nothing to preserve.

## Debugging a conversion: read the source the driver got

`SPRING_DBG_SHADERSRC=<dir>` writes every Lua shader the driver is handed into
that directory, numbered in compile order. Use it the moment an error does not
match the file on disk, because normalisation rewrites the source — `#version`
clamped, `#extension` filtered, `layout(binding)` lifted, `#define`s injected —
and the driver's line numbers refer to the *result*.

This found a bug nothing else would have. `RewriteGlslLayoutBindings` used to
search for `"layout"` anywhere and then for the next `(` anywhere after it, so
the word **"layout" in a prose comment** paired with the parentheses of the next
real declaration and deleted everything between them. The shader still compiled
as far as the driver was concerned; it just no longer declared the uniform, and
the error appeared as that uniform being undeclared a dozen lines further down.
The rewriter now skips comments and requires `(` to follow the token directly,
but the general shape of that failure — *normalisation silently ate something* —
is worth suspecting whenever an error names an identifier you can see declared.

To map a driver line number back: find the last `#line N` above it in the dump,
and count forward from the file line after it.

## Getting the pristine original out of the archive

BAR arrives through rapid, which does not unpack: the archive is an `.sdp` index
in `<write-dir>/packages/` and the file bodies are gzipped blobs in
`<write-dir>/pool/`, named by md5. `devtools/macos-port/bar-extract.py` reads the
index and pulls one file out:

```
./devtools/macos-port/bar-extract.py build-all LuaUI/Shaders/foo.vert.glsl > foo.glsl
./devtools/macos-port/bar-extract.py build-all --list | grep -i sensor
```

Paths in the index are lowercased, and the script matches case-insensitively, so
type whatever casing the widget uses. Regenerating every patch at once — after a BAR update, or after editing a
deployed file — is `devtools/macos-port/regen-patches.sh`. It extracts each
original, diffs it against the deployed override, and dry-run re-applies the
result, so a file BAR has renamed or restructured is reported rather than
silently producing an empty patch.

## Reusable snippets

`devtools/macos-port/snippets/` holds the two replacement blocks verbatim, with
their rationale:

- `model-uniforms-texbuf.glsl` — replaces the engine `UniformsBuffer` SSBO;
  then rewrite every `uni[i]` as `fetchUni(i)`
- `model-transforms-texbuf.glsl` — the transform equivalent. **Read its note
  first:** this conversion is already done centrally in `luashader.lua`, and the
  element type is the 32-byte quaternion `Transform`, not a `mat4`. Several BAR
  shaders still carry a dead `mat4 UnitPieces[]` declaration behind
  `#if USEQUATERNIONS == 0`; converting that branch is wasted work.

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

The tracker gate above can only be fixed by an **archive** widget, so the
overrides ship as a `.sdd` archive rather than write-dir files.
`devtools/macos-port/bar-overrides/` is the source of truth — a `modinfo.lua`
plus one patch per overridden file, each against the file as the BAR archive
ships it.

Deploy by building `<write-dir>/games/bar-macos.sdd/`: extract each original with
`bar-extract.py`, apply its patch, and place the result at the same path.
`modinfo.lua` declares `depend` on the stock BAR archive, so BAR is still loaded
underneath and only these files are shadowed. The deployed tree is gitignored
and BAR rewrites parts of the write dir, so **the patches are the durable copy,
not the deployed files.**

### Loading it from the menu: `ExtraMutators`

An archive like this is a mutator, and a mutator is normally named by the start
script — which means a game started from the Chobby menu would not use it, since
Chobby writes BAR's own gametype. The engine therefore has a config key
(`rts/Game/GameSetup.cpp`, commit `a73d1a52d4`):

```
ExtraMutators = BAR macOS overrides 1
```

`LoadMutators` appends these semicolon-separated archive names after whatever
`MUTATOR%i` entries the script carried, so menu play picks the overrides up too.
Single-player only, by design: it changes the content a client loads without
telling the server.

The earlier attempt — redirecting the rapid tag so the override archive
masqueraded as BAR — is **not** the way. It broke Chobby's skirmish modal, since
`VFS.UseArchive(gameName)` then opened a 4-file archive and `Include
"modoptions.lua"` failed. `ExtraMutators` adds an archive instead of
impersonating one.

### Three things that cost time getting this working

- **The gametype string is `name .. " " .. version`,** not `name`. This archive
  registers as **`BAR macOS overrides 1`**; naming it `BAR macOS overrides`
  fails with `Dependent archive ... not found`, which reads as if the archive
  were missing rather than misnamed. Check `cache/ArchiveCache22.lua` for the
  registered `name` when in doubt.
- **`depend` must name the BAR archive exactly**, version included
  (`Beyond All Reason test-31765-420384b`). It changes whenever BAR updates, so
  this is the line that will need maintaining.
- **The archive cache can hide a modinfo mistake.** `cache/ArchiveCache22.lua`
  is keyed on mtime; if a scan looks like it did nothing, delete that file and
  re-run.

### Verified

Playing a skirmish from the menu: no `Missing capabilities: API Unit Tracker`
line, health bars appear on damaged units, and the AI fights back. Failing
shader programs 29 -> 12, with every remaining one accounted for in the table at
the top of this file.
