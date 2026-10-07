# Plan: renderer deko3d para SwitchWakerHD

Plan redactado el 2026-10-07 por un agente de planificación (Fable) a partir del código (`dev` 30fd148,
round 36), del prototipo de uam (`~/Documents/uam-proto`) y de los logs de hardware. **[V]** = verificado en
código o logs; **[S]** = supuesto a confirmar. Estado: P0 y P1 hechos y confirmados en hardware (rama `deko3d`); interfaces de P2 definidas; ver Estado y «P2: carriles».

## Estado (2026-10-07, rama `deko3d`)

Integrados `dk-uam`, `dk-convert`, `dk-tools` y `dk-p1` sin conflictos. Compilan `build/switch/wwhd.nro` (GL, por
defecto, sin cambios de comportamiento), `build/switch-dk/wwhd_dk.nro` (`WWHD_RENDERER=DEKO3D tools/switch/build.sh`,
con libdeko3dd), `build/uamtest2/uamtest2.nro` (`tools/switch/uamtest/build.sh`) y la herramienta de host
`build/dksh_cache/dksh_cache` (`tools/switch/dksh_cache/build.sh`).

**Confirmado en hardware (2026-10-07):** uamtest2 1561/1561 DKSH idénticos al Mac (media 70 ms, heap +24 KB por
compile); `wwhd_dk.nro`: patrón, orientación, test de profundidad y overlay correctos; el juego corre detrás a 30 fps
con GX2 sin ejecutar y llega a Outset (por el sonido).

**P0 (hecho en el Mac):**
- uam 1.1.0 vendorizado (`runtime/third_party/uam`, target `uamlib`, API `uam_api.h`, parches en `PATCHES.md`): log por
  callback, sin `exit/abort`, DKSH a memoria, frontend residente (~20 % más rápido) y arreglo de una lectura sin
  inicializar en el scheduler GM107 de nv50_ir (antes 88/1561 DKSH dependían del heap). 1561/1561 idénticos al CLI;
  valgrind limpio. Switch: 2,0 MB de texto.
- `glsl_to_deko` (`gfx/deko/glsl_convert.cpp`, `kConvertRevision`): 7680/7680 de la cosecha convierten y compilan;
  los 2654 VS conservan el `SET_POSITION` z 0..1; máximo 4 UBOs y 8 samplers por stage. Cache de consola 1561/1561.
- `dksh_cache` (WGS1 → `WDK1`, `gfx/deko/shader_files.cpp`): cosecha 7680/7680 en 37 s, uam host p50 2,8 ms, p99 26 ms;
  `shadercache_dksh.bin` 13,2 MiB (uamId `007b6d8c06ba645b`), determinista. **Cobertura (riesgo 1):** la cosecha sin
  la cache de la consola cubre 96,2 % de las fuentes de la consola (1501/1561) y 95,7 % de los pares; se espera que
  falte ~4-5 % → compilación en consola para ese resto.

**P1 (compilado):** `gfx/switch_renderer.h` (`gfxsw`) para los hooks Switch (GL los reenvía, sin cambios);
`Api::Deko3D` por defecto cuando está compilado; dispositivo, cola, memblocks (stream ring 4 × 32 MB, comandos
4 × 4 MB con primer trozo explícito por frame, heap de imágenes en chunks de 64 MB, código 32 MB, descriptores),
swapchain 3 × RGBA8 1280×720, patrón de orientación/profundidad, contador FPS y overlay ImGui. Todas las entradas GX2
existen pero solo cuentan (el juego corre, la pantalla muestra solo el patrón). Errores: `[dk]` + `log_flush()` antes
de cada creación, `dkQueueIsInErrorState` antes de acquire/submit/present → `fatal()` legible.

**Pruebas en hardware para el propietario** (ficheros listos en `build/deko3d-out/` del checkout principal):
1. `uamtest2.nro` → `sdmc:/switch/uamtest/uamtest2.nro`, con `shadercache_dksh.bin` → `sdmc:/switch/uamtest/` (para la
   comparación bit a bit). Usa `sdmc:/switch/wwhd/shadercache_gl.bin` de la consola. Ejecutar a reloj stock, esperar
   al final (~2,5 min para ~1561 shaders; `+` sale). Traer `sdmc:/switch/uamtest/uamtest2.log` y
   `shadercache_dksh_switch.bin`. Mirar: `ok`/fallos (esperado 1561/1561), media/p50/p99 por shader (antes 95/77/403
   ms; el frontend residente debería bajarla), crecimiento del heap por compile y ausencia de `LEAK?`, mayor arena
   (cota de memoria de uam), y 0 diferencias de DKSH y mapas de bindings frente al Mac.
2. `wwhd_dk.nro` → `sdmc:/switch/wwhd/` junto a `wwhd.nro` (título propio "SwitchWakerHD (deko3d)"; comparte carpeta,
   `settings.ini`, `env.txt`; `wwhd.log` se sobrescribe, cada sesión queda en `logs/`). Arrancar, dejar 10 min, abrir el
   menú con `-`, cambiar perfiles. Hacer una foto con el botón de captura del mando. El patrón debe verse como dice su
   leyenda: texto derecho, caja roja arriba a la izquierda y verde abajo a la derecha, fondo azul arriba, barra negro →
   blanco, cian delante de magenta, cuadro gris vacío (sin naranja). Si se ve espejado en vertical, el convenio de y
   está mal (sección 2). Log: líneas `[dk]` de creación del dispositivo, memblocks, swapchain y heap; cada 5 s fps
   (esperado ~60 en el patrón; los recuentos GX2 por frame muestran que el juego avanza), memoria de comandos por frame,
   stream y heap estables. Cualquier `FATAL`/mensaje `deko3d ...` en las últimas líneas es el fallo; los dos sticks
   pulsados registran el tiempo de un frame. `wwhd_dk.nro` va con libdeko3dd (más lento): no medir rendimiento con él.
3. `wwhd.nro` (GL, de esta rama) solo como control de no regresión: debe comportarse igual que el de `dev`.


## P2: carriles (interfaces en la rama `deko3d`)

Tres ramas (`p2-surf`, `p2-shader`, `p2-draw`) parten de `deko3d` (`git merge deko3d`) y trabajan en paralelo, cada
una en sus ficheros. Las interfaces están en `gfx/deko/dk_surfaces.h`, `dk_shaders.h` y `dk_draw.h`; los `.cpp`
son stubs que compilan y mantienen el comportamiento de P1 (GX2 contado, no ejecutado; lo que tendría que devolver un
recurso real hace `fatal()` legible). Cambiar una cabecera ajena: solo añadir, y avisarlo al integrar.

| Carril | Ficheros (propios) | Qué implementa |
|---|---|---|
| `p2-surf` | `surfaces.cpp`, `formats.cpp`, `descriptors.cpp` (+ nuevos con prefijo `surf_`) | `dk_surfaces.h`: formatos (port de `vulkan/formats.cpp`), surfaces/DkImage/views, detiling y uploads por el stream ring, targets, clears, copias/blit, scan (`copy_to_scan`, `scan_flush`, `present_source`), GuestRanges/`invalidate`, slots de descriptores y cache de samplers, escala interna (stubs a 1 hasta P3) |
| `p2-shader` | `shaders_dk.cpp` (+ nuevos con prefijo `shader_`; `glsl_convert.*`, `shader_files.*` si hace falta) | `dk_shaders.h`: fetch shader, hashes, `translate` con memo (port de `gl/shaders.cpp`), worker uam, caches WDK1/WGS1, mapas de bindings por recurso GX2, `pack_uniforms` (port de `vulkan pack_uniforms_into`), stats |
| `p2-draw` | `draw.cpp`, `backend.cpp`, `shaders/present_*.glsl` (+ nuevos con prefijo `draw_`) | `dk_draw.h`: `draw()` completo (memo/combos, índices, cache de estado → structs `Dk*State`, vértices, texturas, UBOs), `submit_commands`, pase de presentación del juego (sRGB, grade), stats/hitch/trace, GamePad skip |

Ficheros comunes (`dk.h`, `memory.cpp`, `overlay_dk.cpp`, `glsl_convert.h`, `shader_files.h`): solo en la integración.
`R.perf` (dk.h) ya tiene los campos de los tres carriles; cada uno suma solo a los suyos.

**Contratos:**
- **Hilos.** El render thread (el de GX2) es el único que graba comandos deko3d, toca `R`, las surfaces, los `Shader`,
  la memoria de código y la memoria del guest. El worker de uam (uno, pila 8 MB, prioridad por debajo del juego) recibe
  GLSL ya convertido por `glsl_to_deko` y devuelve bytes DKSH o un error; nunca toca objetos deko3d.
  `shaders_init` (hilo principal, en `init` tras `memory_init`) arranca el worker y carga las caches;
  `save_shader_cache` en `shutdown`.
- **Frame.** `begin_commands()` (dk.h, backend.cpp) abre el frame si no está abierto: fence del slot, stream y memoria
  de comandos, barrier, sets de descriptores, y luego `surfaces_frame_start`, `shaders_frame_start` (carga los DKSH
  terminados), `draw_frame_start`. La tabla `Backend` la llama antes de cada entrada GX2 (`guarded`), y `present()`
  por si ninguna grabó. `present()` cierra el frame (`frame_end`, submit, present). `submit_commands(why)` envía lo
  grabado a mitad de frame (GX2Flush, GX2DrawDone, memoria de comandos grande) y se sigue grabando en el mismo frame.
- **Stream.** `stream_upload` / `stream_guest` (dk.h, memory.cpp, ya implementados): slice por frame de 32 MB, sin
  wrap; `stream_guest` deduplica por dirección mientras `R.streamGen` no cambie (GX2DrawDone, `invalidate` de
  atributos/uniforms). Slice vacío (gpu 0) = lleno y logueado: el llamador salta el draw (`perf.streamFullSkips`).
- **Descriptores.** Los asigna y recicla solo `p2-surf` (`descriptors.cpp`): imágenes 64..8191, samplers 16..1023;
  0..63 / 0..15 son del renderer (overlay 1-63 y sampler 0; present: imagen 0 y samplers 1-2). Se escriben con
  `dkCmdBufPushData` (ordenado con los comandos; nunca la CPU sobre un descriptor que la GPU puede leer); un slot
  liberado vuelve a usarse cuando la GPU termina el frame. `commit_descriptors()` antes del draw que los usa.
- **Texturas en el draw.** Por unidad: `sampled_texture(words)` → `upload_surface` → `sampled_view_id` +
  `sampler_id(samplerWords, compare, integer)` (cache del draw por palabras y `R.surfaceEpoch`, como GL);
  `feedback_copy` si la textura es un target del draw; `commit_descriptors`; `dkCmdBufBindTextures(stage,
  sh->textureSlot[unit], dkMakeTextureHandle(view, sampler))`. Unidad sin surface: `null_image_id()`.
- **UBOs y shaders en el draw.** `translate(VS)`, `translate(PS)`; no `ready()` → `shader_wanted` y skip (el memo de
  combos no recuerda el fallo). Bloques del guest con `stream_guest` en `sh->uboSlot[i]` (tamaño `sh->uboBytes[i]`, o
  el del guest hasta 64 KiB con cola de ceros redondeada a 256); el ufBlock con `pack_uniforms(stage, *sh, regs,
  scale)` en `sh->bindings.ufBlockSlot`. `dkCmdBufBindShaders` con `&sh->dk`.
- **Estado.** Todo lo que fuera de `draw()` ata targets, shaders, viewports/scissors o estado 3D (clears, present,
  overlay) llama a `forget_state()` (dk_draw.h). Las copias del motor 2D no. Uploads y copias dejan su resultado
  visible con su propio barrier; el draw no añade ninguno por ellos. Los clears ponen su scissor a todo el target.
- **Errores.** Cada fallo con línea `[dk] ...` que nombre el recurso (dirección, formato, hash); `fatal()` para lo que no
  puede continuar; `log_flush()` antes de cada creación deko3d; `check_queue` antes de submit/acquire/present.

## 0. Hechos que condicionan el diseño

- **[V]** `render::Backend` (`runtime/src/gfx/renderer.h`) es una tabla de punteros; añadir un backend es un
  `Api` nuevo, `deko3d_backend()` bajo `WWHD_HAS_DEKO3D` y `select_decompiler_api` en `gx2/decompiler_glue.cpp`.
- **[V]** El decompilador de Cemu emite el mismo GLSL para todos los renderers (ambas ramas `#ifdef VULKAN / #else`
  en el fuente) y rellena `resourceMappingGL` y `resourceMappingVK` a la vez. El backend deko3d puede usar el modo
  OpenGL del decompilador: los hashes de `shadercache_gl.bin` (WGS1), incluida la cosecha, siguen válidos.
- **[V]** uam no es reentrante (`static struct gl_context gl_ctx` en `glsl_frontend.cpp`; crash con 3 hilos aun con
  mutex reales): **un único worker de compilación**.
- **[V]** uam en consola (1020 MHz, un hilo): 1561/1561, media 95 ms, p50 77, p90 204, p99 403, máx 1077. Host: p50 9 ms.
  DKSH ~1,8 KB/shader.
- **[V]** `DekoCompiler::OutputDksh` solo escribe a fichero (hace falta variante a memoria); `libuamlib.a` del prototipo
  es un thin archive: compilar uam desde fuente en CMake.
- **[V] Defecto del convertidor** `cemu_to_deko.py`: descarta la rama VULKAN del bloque de macros de los VS (porque
  contiene `gl_VertexIndex`) y con ella el `SET_POSITION` que remapea z a 0..1. En deko3d el rango de profundidad es
  un flag de dispositivo: el convertidor definitivo conserva la rama VULKAN y borra solo los dos `#define
  gl_VertexID/gl_InstanceID`. El GLSL real usa `\r\n`.
- **[V]** Límites deko3d: 16 UBOs y 32 texturas por stage, UBO máx 64 KB (cabe la paleta de huesos), 16 vertex
  buffers, 32 atributos, 8 render targets; código alineado a 0x100 con 0x400 bytes inutilizables al final del memblock.
- **[V]** Todos los formatos GX2 que mapean `gfx/gl/formats.cpp` y `gfx/vulkan/formats.cpp` tienen `DkImageFormat`;
  las conversiones CPU actuales se conservan al principio.
- **[V]** Hardware a reloj stock: 4359 draws → 26 fps, render thread 751 ms/s + hilo GL de Mesa 266 ms/s (state 255,
  submit 35); Windfall primera visita 18,9 s congelado; heap libre 1643 MiB tras el setup GL, 1320 tras el cache
  (323 MiB de programas Mesa).
- **[V]** La cosecha (C) llevaba 7578 fuentes / 5691 programas: un cache DKSH completo rondará 8-9k shaders, ~16-18 MB.

## 1. Arquitectura

- `Api::Deko3D` ("deko3d"), `deko3d_backend()`, `default_api()` lo prefiere si está compilado.
- Decompilador en modo OpenGL (mismas claves y hashes); el backend lee `resourceMappingVK` / `uniformOffsetsVK`.
- `gx2/gx2_core.cpp:95`: la máscara de registros shader-dirty del GL también para deko3d.
- Mover los hooks Switch que hoy llaman a `gfxgl::` (settings, input capture, overlay) a una cabecera neutra
  `gfx/switch_renderer.h` (namespace `gfxsw`) que implementa cada backend.
- Nuevo `runtime/src/gfx/deko/` (namespace `gfxdk`), **copia estructural del backend GL** (ya afinado para Switch)
  con las llamadas GL sustituidas; sin abstraer código común (GL debe seguir intacto como alternativa).

| Pieza | Fuente | Qué cambia |
|---|---|---|
| Claves, traducción, cache WGS1, budget (A), writer, precarga | `gl/shaders.cpp` | No hay link: "programa" = VS y PS listos; el budget pasa a cola del worker |
| Memo/combos, índices, targets, feedback copies, trace/probes, HUD, GamePad skip | `gl/draw.cpp` | Bindings GL → `dkCmdBufBind*`; cache de estado → structs `Dk*State` |
| Detiling, sparse hash, pool, GuestRanges | `gl/surfaces.cpp` | `DkImageLayout` + `dkCmdBufCopyBufferToImage` |
| DynamicRes, GpuBusy, GpuPasses, stats, present, grade, FPS, main loop | `gl/backend.cpp` | Fences → `DkFence`; timestamps → `dkCmdBufReportCounter` |
| Overlay ImGui | `gl/overlay_gl.cpp` | Shaders DKSH precompilados |
| Formatos | `vulkan/formats.cpp` | `VkFormat` → `DkImageFormat` |
| Imágenes, views, uploads | `vulkan/surfaces.cpp` | Modelo directo |
| `pack_uniforms_into` (layout del `ufBlock`) | `vulkan/shaders.cpp` | Casi literal |
| Ring de submissions con fences y retirada | `vulkan/backend.cpp` | Modelo del ring de command buffers |

- **GL como alternativa en tiempo de compilación**: dos NROs del mismo árbol (`wwhd.nro` deko3d, `wwhd_gl.nro` GL),
  misma carpeta, mismos `shadercache_gl.bin`, `settings.ini`, `env.txt`. CMake `WWHD_RENDERER=DEKO3D` (Switch sigue en
  `OPENGL` por defecto hasta P5): link `deko3d` (+`deko3dd` en Debug) + `uamlib`, sin EGL/glapi/drm_nouveau/glad ni los
  `--wrap` de Mesa. La imagen `devkitpro/devkita64` ya trae `libdeko3d.a` y `uam`.

## 2. Shaders

```
regs → translate() (GL) → GLSL Cemu → hash → fuente WGS1
  ¿DKSH en memoria? → listo
  ¿DKSH en cache SD/offline? → cargar → listo
  si no → glsl_to_deko() → cola del worker uam → DKSH → (render thread) code memblock + dkShaderInitialize → listo
draw(): VS o PS no listo → skip (budget A); el memo de combinaciones no recuerda el fallo
```

- `glsl_to_deko()` en C++ (`gfx/deko/glsl_convert.cpp`, compilable también en host): rama VULKAN completa, sin los
  `#define gl_VertexID/InstanceID`, sin `set =`, macros de layout expandidas, UBOs (<16) y samplers (<32) renumerados
  por stage con mapa devuelto, `#version 460`, `invariant gl_Position`, `\r\n`.
- Sin link (stages separados). Inputs que el VS no escribe: variante del VS con esas salidas (como `link()` hoy) **[S]**.
- Worker único: stack 8 MB, prioridad por debajo del juego (0x3C), cores 0/2, `glsl_frontend_init()` una vez; cola con
  prioridad (el shader que más draws saltó; VS y PS del par juntos). ~10 shaders/s → ~190 ms por par.
- Caches DKSH: en memoria; en SD `shadercache_dksh.bin` (`WDK1`: uamId, stage, glslHash, bytes; derivado de WGS1);
  **offline** generado en el Mac desde el `shadercache_gl.bin` cosechado (~8-9k shaders, 1-3 min) y distribuido con el
  NRO; carga al boot de ~18 MB en segundos.
- Fetch shaders → `DkVtxAttribState`/`DkVtxBufferState` (todo `Uint`, el GLSL decodifica); sin rebasing ni multi-draw
  al principio (existían para Mesa). Índices nativos, incl. TriangleFan/Quads/LineLoop (P4).
- Estado: la cache `gs` del GL traducida 1:1 a `DkRasterizerState`, `DkDepthStencilState`, `DkBlendState[8]`,
  `DkColorState`, `DkColorWriteState`, viewport/scissor escalados.
- Dispositivo `OriginUpperLeft | DepthZeroToOne` **[V]** (fuente de deko3d 0.5.0): origen de ventana e imágenes arriba a
  la izquierda, z de clip 0..1, pero **la y del clip space apunta hacia arriba como en OpenGL** sea cual sea el origen
  (`Primer.md`; `dkCmdBufSetViewports` usa `scaleY = -h/2` con `OriginUpperLeft`, así que y = +1 es la fila 0).
  `YAxisPointsDown` llegó después de 0.5.0 (commit e4e89f4) y no existe en la imagen de devkitPro. Para portar la
  matemática y-abajo de `gfx/vulkan/draw.cpp` hay que **negar y**: en P2 el `SET_POSITION` de la rama VULKAN niega
  `gl_Position.y` (los shaders propios de P1 ya lo hacen); nunca viewports de altura negativa (`viewportH` se calcula
  como `uint32_t` y desborda). Los scissors siguen en coordenadas de ventana desde arriba. Confirmar con el patrón de P1.
- Errores: libdeko3d (release) aborta con 2359-xxxx sin llamar al callback ante cualquier fallo (creaciones, uso de una
  cola en estado de error: submit, acquire, present, flush); las creaciones nunca devuelven null. El backend loguea y
  hace `log_flush()` antes de cada creación, comprueba `dkQueueIsInErrorState` antes de acquire/submit/present y
  termina con `fatal()`. Mientras el backend es nuevo se enlaza libdeko3dd también en Release
  (`WWHD_DEKO3D_DEBUG_LIB`, ON por defecto): sus errores llegan a `debug_message`, que hace `fatal()` legible.
- Memoria de comandos: `dkCmdBufClear` (0.5.0) rebobina al inicio del último trozo añadido y no pide memoria nueva; cada
  frame añade explícitamente el primer trozo de su slot tras la fence (como `CCmdMemRing` de deko_examples).
- uam vendorizado en `runtime/third_party/uam` (MIT) con parches: pthread en `c11/threads.h`, `WriteDksh` a memoria,
  log por callback, sin `exit/abort`. Bison/flex generados y commiteados. **Medir su memoria en P0** [S].

## 3. Memoria y recursos

| Memblock | Tamaño | Uso |
|---|---|---|
| Stream ring ×4 (CpuUncached, GpuCached) | 4 × 32 MB | Vértices, índices, UBOs, `ufBlock`, staging, overlay; fence por frame |
| Image heap (Image) | chunks de 64 MB, suballocador | Texturas y render targets |
| Code | 32 MB, bump | DKSH del juego y embebidos |
| Descriptores | 8192 imágenes + 1024 samplers | `DkResHandle` por draw; slots reciclados por fence |
| Command memory | ring 4 × 4 MB + callback | Por frame |
| Queries | 64 KB | Timestamps de pases |

Ahorro esperado frente a GL: ~320 MiB de programas Mesa y gran parte de los ~140 MiB de EGL/Mesa.

- Surfaces: `DkImageLayout` + `DkImage` + views (swizzle, formato, mips/layers). Uploads por el stream ring +
  `dkCmdBufCopyBufferToImage` + barrier. Render targets con `HwCompression` (en depth, probar: bug de nouveau del
  round 26). Depth: conservar al principio D24S8 → ZF32_X24S8 y D24 → R32F. Copias con `dkCmdBufCopyImage` /
  `dkCmdBufBlitImage` (depth siempre por blit). `GX2Flush/DrawDone` → `dkQueueFlush`. Retirada por frame.

## 4. Presentación

- Swapchain de 3 imágenes RGBA8 sobre `nwindowGetDefault()` a 1280×720, intervalo 1; relaxed vsync sigue en
  `gx2_core.cpp`. Docked 1080p en P4.
- Present = port de `present_program()` (sRGB + ajustes de imagen). Shaders internos (present, FPS, ImGui)
  compilados offline con el `uam` de devkitPro y embebidos.
- Overlay: port de `overlay_gl.cpp`, llamado desde `present()` como hoy. Dumps/capturas por
  `dkCmdBufCopyImageToBuffer`. Línea de stats `[dk]` con los mismos campos que `[gl]`.

## 5. Fases

- **P0 — uam vendorizado y herramientas DKSH (2-3 días).** CMake `uamlib` host/Switch con parches, `glsl_to_deko` en
  C++ con el arreglo de `SET_POSITION`, herramienta `dksh_cache` (WGS1 → WDK1), NRO de prueba con memoria medida.
  *Salida:* 100 % de la cosecha compila en host; 1561/1561 en consola; memoria de uam; tasa de fallo del cache offline.
- **P1 — dispositivo, clear, present, overlay (3-5 días).** Esqueleto `gfx/deko`, memblocks, ring, swapchain, present
  con patrón de orientación/profundidad, FPS, overlay funcional, GX2 como no-op. *Salida:* arranca, overlay y
  perfiles funcionan, 10 min sin crash.
- **P2 — pantalla de título (2-3 semanas).** Surfaces, uploads, descriptores, draw completo, clears, copias, scan,
  worker + caches + budget. *Salida:* título idéntico a GL, 0 skipped con cache caliente, µs por draw. **Go/no-go.**
- **P3 — paridad en juego (1-2 semanas).** Outset, bosque, Windfall, Fortaleza; feedback/depth copies, resolución
  dinámica, GamePad skip, varyings no escritos, focos. *Salida:* escenas idénticas, 30 min sin crash, Windfall sin
  frame > 100 ms con cache offline.
- **P4 — rendimiento y features (1-2 semanas).** Binds redundantes, push constants, primitivas nativas, decode en
  worker, depth sin copia, docked 1080p. *Salida:* render thread ≤ ~650 ms/s a stock; fps ≥ GL en cada ventana.
- **P5 — deko3d por defecto (2-3 días).** `wwhd.nro` deko3d, `wwhd_gl.nro` una o dos releases más; documentación.

Total: ~6-9 semanas de trabajo efectivo, más ciclos de prueba en hardware.

## 6. Riesgos (ordenados)

1. uam de un hilo a ~95 ms/shader: si el cache offline falla mucho, pop-in continuo. Medir en P0 qué fracción de los
   hashes de la consola falta en la cosecha. Plan B: un `gl_context` por hilo en `glsl_frontend.cpp`.
2. Convenciones globales de dispositivo frente al `glClipControl` por draw: patrón de prueba en P1, comparación del
   título en P2.
3. Barriers explícitos: empezar conservador (Full tras cada cambio de targets y upload), relajar midiendo.
4. Memoria de uam en consola (fuga = bloqueante): 1000 compiles en P0.
5. Semántica de uam (división entera, inputs no escritos, invariant): grep de los GLSL cosechados y capturas en P3.
6. Coste del worker sobre los hilos del juego: medir audio gaps y `[main]` en una primera visita.
7. Muchos memblocks/descriptores: suballocar desde P1.
8. Depurar sin GPU debugger: `libdeko3dd.a`, `dkQueueIsInErrorState` por frame, trace/probes, comparación con GL.
9. Formatos: ninguno falta [V].
10. Cosecha en marcha: herramienta offline re-ejecutable.

## 7. Ganancias y go/no-go

- Congelaciones por shaders: 18,9 s (Windfall) y 24,6 s por sesión → 0 con cache offline; boot sin compilar.
- CPU **[S]**: render thread 830-1000 → 550-700 ms/s y desaparece el hilo de GL de Mesa (266-393 ms/s); vistas
  limitadas por CPU de 24-27 → 27-30 fps a stock. No mejora la GPU (vistas GPU-bound siguen con resolución dinámica).
- Memoria: ~400 MiB más de margen.
- Estabilidad: sin parches de Mesa, sin el crash del PBO, sin esperas al hilo de GL, sin la cache Mesa de 57 MB.

**Go/no-go al final de P2:** seguir si el tiempo de render thread por draw baja ≥ 25 % frente a GL, el título no
regresa, el cache offline falla ≤ ~20 % y la memoria de uam es estable. Parar (GL + budget A + cache offline) si uam
fuga o pide > 150 MB por compile, el ahorro por draw es < 15 %, o las convenciones de profundidad/orientación no se
igualan tras dos semanas extra de P2.
