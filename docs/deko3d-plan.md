# Plan: renderer deko3d para SwitchWakerHD

Plan redactado el 2026-10-07 por un agente de planificación (Fable) a partir del código (`dev` 30fd148,
round 36), del prototipo de uam (`~/Documents/uam-proto`) y de los logs de hardware. **[V]** = verificado en
código o logs; **[S]** = supuesto a confirmar. Estado: P0 y P1 hechos y confirmados en hardware; P2 probado en hardware (llega a Outset); P3 parte A (capturas, contador FPS, texturas, features) integrada, pendiente de prueba en hardware; ver Estado.

## Estado (2026-10-07, rama `deko3d`)

Integrados `dk-uam`, `dk-convert`, `dk-tools`, `dk-p1` y los tres carriles de P2 (`p2-surf`, `p2-shader`, `p2-draw`,
sin conflictos). Compilan `build/switch/wwhd.nro` (GL, por defecto, sin cambios), `build/switch-dk/wwhd_dk.nro`
(`WWHD_RENDERER=DEKO3D tools/switch/build.sh`, con libdeko3dd; `WWHD_DEKO3D_DEBUG_LIB=OFF` enlaza la librería release),
`build/uamtest2/uamtest2.nro` y la herramienta de host `build/dksh_cache/dksh_cache`.

**P0 y P1 confirmados en hardware (2026-10-07):** uamtest2 1561/1561 DKSH idénticos al Mac (media 70 ms, heap +24 KB
por compile); `wwhd_dk.nro` de P1: patrón, orientación, test de profundidad y overlay correctos; el juego corría detrás
a 30 fps con GX2 sin ejecutar y llegaba a Outset (por el sonido).
- P0: uam 1.1.0 vendorizado (`runtime/third_party/uam`, `uam_api.h`: un solo hilo, pila 8 MB); `glsl_to_deko`
  (`gfx/deko/glsl_convert.cpp`, 7680/7680 de la cosecha); formato `WDK1` (`gfx/deko/shader_files.*`). Cobertura de la
  cache offline: 96,2 % de las fuentes de una consola (riesgo 1: el ~4 % restante se compila en la consola).
- P1: dispositivo `OriginUpperLeft | DepthZeroToOne` (la y del clip apunta ARRIBA: los VS niegan y), anillos con
  fences por frame (`memory.cpp`), swapchain 3 × RGBA8 1280×720, contador FPS, overlay ImGui. El patrón de prueba
  queda tras `WWHD_DK_TEST_PATTERN=1`.

**P2 (integrado y compilado; SIN probar en hardware):** el juego dibuja con deko3d. Objetivo: pantalla de título y
menús correctos (el juego en sí es P3).
- **Shaders** (`shaders_dk.cpp`): port de `gl/shaders.cpp` (mismas claves y hashes, memo, `shadercache_gl.bin`) →
  `glsl_to_deko` → uam en un worker (pila 8 MB, prioridad 0x3C, núcleos 0 y 2; autoprueba al arrancar: línea
  `[dk] shader worker: uam ready` o `SELF-TEST FAILED`) → DKSH a la memoria de código en el render thread (64 cargas
  por frame, `WWHD_DK_SHADER_BUDGET`; 0 = el draw espera al compilador). Mientras un shader está pendiente sus draws
  se saltan (como GL sin shader). Al arrancar, barra de progreso mientras `shadercache_dksh.bin` (offline) y
  `shadercache_dksh_local.bin` (lo compilado en esta consola) se cargan enteras. `kConvertRevision` 2 (SET_POSITION
  niega y): **uamId nuevo `c6088028d9e60f00`**; la cache vieja (`007b6d8c06ba645b`) se rechaza con una línea `[dk]`.
  `pack_uniforms` es el port de `vulkan pack_uniforms_into`.
- **Surfaces** (`surfaces.cpp`, `formats.cpp`, `descriptors.cpp`): port de `gl/surfaces.cpp` (búsqueda, caches de
  targets, detiling, hash disperso, GuestRanges, copias, `ss_reset`) con imágenes, vistas y formatos de Vulkan. Subidas
  por un anillo de staging de 32 MiB; clears con viewport/scissor completos; copias por el motor de copia o el 2D (depth,
  escaladas); compresión HW en los targets de color (`WWHD_DK_RT_COMPRESSION=0` la quita; la de depth está apagada,
  `WWHD_DK_DEPTH_COMPRESSION=1`). Autocomprobación de los layouts de todos los formatos en el primer frame. Escala
  interna fija a 1 (P3). Barriers completos en cada subida/copia (correcto pero caro: afinarlos es P4).
- **Draw** (`draw.cpp`): port de `gl/draw.cpp` (memo y combos, índices convertidos en CPU con cache por frame, recorte
  de vértices, caches de targets y texturas, feedback copies, GamePad skip, trace) a los structs `Dk*State` con cache
  de estado; un submit cada 256 draws (`WWHD_DK_SUBMIT_DRAWS`). Viewport: con `PA_CL_VPORT_YSCALE` < 0 (lo normal) un
  swizzle NegativeY deshace la negación del VS; la cara frontal es la de Latte (`WWHD_DK_FLIP_FRONT=1` la invierte, por
  si el culling sale al revés). Barrier `Fragments` en cada cambio de targets (`WWHD_DK_PASS_BARRIER=0` lo quita).
  Las variables `WWHD_DK_*` aceptan también el nombre `WWHD_GL_*` de GL (`TRACE_FRAMES`, `TRACE_DRAWS`, `SKIP_GAMEPAD`...).
- **Presentación** (`backend.cpp`): cada swap presenta el buffer de TV que el juego copió (`copy_to_scan`: el propio
  buffer si nada lo reescribió, si no la copia) ajustado a 16:9, con el grade de imagen y la codificación sRGB de
  `gl present_program` (si la imagen es sRGB, el shader deshace la decodificación del muestreo: mismos bytes que GL con
  `GL_SKIP_DECODE_EXT`), y encima el contador FPS y el overlay. Negro mientras no haya imagen. GX2Flush y GX2DrawDone
  envían lo grabado (`dkQueueSubmitCommands` + `dkQueueFlush`) sin esperar a la GPU, como GL.
- **Logs** cada 5 s: `[dk] <fps> fps; GX2 per frame...`, `[dk] draws per frame: N executed; skipped ...` (motivos),
  `[dk] shaders: ...` (traducidos, en RAM, de las caches, compilados, fallidos, pendientes, draws saltados),
  `[dk] surfaces: ...` (superficies, memoria, subidas, copias, descriptores), `[hitch]` para frames de más de 55 ms.
- **No portado aún:** FXAA, probes, rect lists (GL tampoco las dibuja), registros 2 de
  `shadercache_gl.bin`. (Escala interna, resolución dinámica, AO, aniso y timestamps por pase: carril P3 `p3-features`, abajo.)

**P3 parte A (integrada en `deko3d` el 2026-10-07; compilada; SIN probar en hardware).** Tras 0ecc13d el propietario
llegó a Outset con `wwhd_dk.nro` y vio (1) algunas texturas mal y (2) el contador FPS roto, sin fotos ni log. Se integraron
tres carriles: `p3-features`, `p3-capture` y `p3-textures` (este último entero redundante con `p3-features`: se queda la
versión de features del AO y de los slots de sampler, más su línea de log de expulsiones). Compilan los tres NRO (GL sin
cambios, deko3d debug y release). `kConvertRevision` no cambió: `shadercache_dksh.bin` de P2 sigue valiendo.
- **Contador FPS:** causa probable encontrada (uam leía siempre `.x` del vector de glifos: "3333"); ver `p3-capture`.
- **Texturas:** auditoría completa de `p3-textures` frente a GL (tabla de formatos, conversiones CPU byte a byte,
  swizzles, detiling, layouts, subidas, samplers, slots, sRGB): sin discrepancias salvo el AO y los slots de sampler
  (arreglados en `p3-features`). Los dos cambios de motor 2D para targets comprimidos se revirtieron (en deko3d la
  compresión es del mapeo de memoria, el motor de copia la lee bien). Sospechas abiertas que solo el hardware decide:
  (a) deko3d 0.5.0 nunca escribe el registro Maxwell 0x56E (framebuffer sRGB): si vale 0, lo dibujado en un target sRGB y
  muestreado después sale **demasiado oscuro**; (b) las texturas depth dan (D,D,D,1) en deko3d y (D,0,0,1) en GL;
  (c) la compresión de los targets (`WWHD_DK_RT_COMPRESSION=0` la descarta). Las capturas PNG deciden entre ellas.

**P3 parte B (2026-10-07, tras las capturas 683 y 1444 del propietario; compilado; SIN probar en hardware).**
Síntomas: el mar plano azul saturado (sin olas ni espuma) y cuadraditos grises sobre la hierba y el puente.
- **Causa (bug de deko3d 0.5.0, `source/dk_image.cpp`):** `dkImageLayoutInitialize` elige la altura de tile con
  `pickTileSize` de 1,5 × la altura en GOBs (l. 436-442): una imagen de 6..8 filas (BC de 24..32 px de alto: 24x24,
  30x30, 32x32, 1024x32, 256x32, 64x32) recibe DOS GOBs. `calcLevelOffset` (l. 90, `adjustTileSize(m_tileH, 8,
  levelHeight)` también en el nivel 0) encoge ese tile a UN GOB para medir el nivel 0 y colocar los mips, pero
  `ImageInfo::fromImageView` (l. 194-202) solo lo encoge con `mipLevelOffset` != 0, y `tic_generate.cpp` (l. 100) pone
  `m_tileH` tal cual: el motor de copia (`BlitCopyEngine`, `gpu_transfer.cpp` l. 70/86), el 2D, los render targets y el
  descriptor escriben/leen el nivel 0 con bloques de dos GOBs. La segunda columna de GOBs del nivel 0 cae en el offset
  del mip 1 (o tras el final de la imagen) y la subida del mip 1 la pisa: en las capturas, los bloques desplazados de
  `tex_*.png` son exactamente el mip 1 (comprobado). Los 8 BC4/BC5 que diferían tienen 6..8 filas de bloques; los que
  coinciden, más. El normal map del mar es `24A2E000` 32x32 BC5_SNORM (draws #3875-3910 de la 1444, t5) y las
  calcomanías del suelo con poly offset (#4018-4021) usan como máscara alfa (vista xxxx) `2DB69000` 64x32 y `2DAD7000`
  256x32 BC4: los cuadraditos.
- **Arreglo:** `image_tile_size_fix` (`memory.cpp`) antes de cada `dkImageLayoutInitialize` de superficies y del overlay:
  si el nivel 0 encogería el tile, `DkImageFlags_CustomTileSize` con el tile ya encogido (un GOB). Así layout,
  descriptor, copias y targets coinciden. Línea `[dk] image tile height: one GOB for ...` (las 16 primeras).
- **Capturas rápidas:** la textura de datos CPU se compara byte a byte con sus datos de subida tras la lectura; solo
  se escriben (con su `_upload.png`) las que difieren (`WWHD_DK_CAPTURE_ALL_TEXTURES=1`: todas, como antes). El hilo de
  render nunca espera al escritor (más de 128 MiB en cola: el fichero se descarta con una línea). Al final,
  `[dk] capture of frame N done in X s: ... written, ... dropped, ... identical`.

**P3 `p3-features` (compilado; SIN probar en hardware):** cada punto con su línea `[dk]` al arrancar.
- **AO** (`draw.cpp`, `pack_uniforms` aoNoise): el arreglo de GL tal cual, `WWHD_AO_MODE` / `WWHD_NO_AO_QUIRK`, modo 2 por
  defecto (`[dk] AO quirk fix: mode N`; la tabla del renderer informa del modo, solo lectura como GL).
- **Varyings flat** con el último vértice, como GL (que nunca llama a `glProvokingVertex`); `WWHD_DK_PROVOKING_VERTEX=latte|first`
  para probar.
- **Aniso 16x** opcional (`WWHD_ANISO=1` o el overlay, Effects; apagado por defecto: GL no tiene) con la condición de Vulkan;
  **LOD** como GL/Mesa: un MIN_LOD mayor que el máximo se intercambia (deko3d subía el máximo al mínimo).
- **Descriptores:** los aciertos de la cache de texturas del draw marcan su sampler como usado (el LRU podía reescribir un
  slot en uso) y una expulsión invalida las caches; el slot del present compara también el tamaño de la imagen.
- **Zcull:** se descarta en cada bind de depth (también con `WWHD_DK_PASS_BARRIER=0`) y tras subidas, copias, blits o
  imágenes nuevas de un depth (`R.zcullEpoch`): deko3d solo lo hace si cambia la dirección del target.
- **GPU passes:** timestamps (`dkCmdBufReportCounter`) cada 30 frames como `GpuPasses` de GL; cada 5 s `[dk] GPU passes:
  ...; frame start: GPU idle X ms`. `WWHD_DK_GPU_PASSES=0` las quita (salvo con resolución dinámica).
- **Escala interna y resolución dinámica:** port de GL (screen_shaped, twin del buffer de TV con el HUD a resolución
  completa, pool de 64 MB, 4 imágenes nuevas por frame, `uf_texNScale`); `WWHD_RES_SCALE`, `WWHD_DYNAMIC_RES` (activa por
  defecto como GL: 0.75..1; `=0` la apaga). El overlay muestra la escala en uso. Cada 5 s `[dk] internal resolution ...`.

**P3, carril `p3-capture` (capturas sin fotos del propietario):**
- **Capturas PNG** (`gfx/deko/capture.cpp`, `dk_capture.h`): los dos sticks pulsados capturan el frame siguiente
  entero en `sdmc:/switch/wwhd/captures/<frame>/`: `frame_<n>.png` (la imagen de TV que muestrea el present, con la
  codificación sRGB como GL), `frame_<n>_window.png` (la imagen del swapchain: barras, contador FPS, overlay),
  `frame_<n>_tv_source.png` (el buffer que el juego copió a scan, si el present leyó la copia), `target_*.png` (cada
  render target de los draws del frame, depth incluido, estirado a gris) y `tex_*.png` (cada textura muestreada, nivel 0
  capa 0, tal como la tiene la GPU) con su `tex_*_upload.png` (los datos del guest decodificados por la CPU como los
  sube `upload_surface`, antes de la GPU: bien aquí y mal en `tex_*.png` = subida, vista o muestreo; mal en los dos =
  formato o decodificación). Una línea `[dk] capture <fichero>: ...` por fichero con formato GX2 y deko3d, tamaño,
  mips/capas, tile mode, y los draws que la usaron (`#n`, la misma numeración que `[trace]   draw #n`) con unidad,
  swizzle de la vista (DST_SEL), formato/num/comp/degamma/endian de las palabras de textura y sampler. Las imágenes se
  copian con `dkCmdBufCopyImageToBuffer` a un bloque CPU-cached de 16 MiB (lotes) tras el present; un hilo convierte a
  RGBA8 (BC1-5, half/float, packed, depth) y escribe el PNG. `WWHD_DUMP_FRAMES=n,...` (imágenes),
  `WWHD_DUMP_TARGETS=n,...` (targets) y `WWHD_DUMP_TEXTURES=n,...` (texturas) hacen lo mismo en frames fijos.
- **Contador FPS:** `text_fsh` leía `glyphs[i >> 2][i & 3]`; uam (glsl_to_tgsi de Mesa) pierde el índice dinámico de
  componente de un vector de UBO y lee siempre `.x` (TGSI: `MOV CONST[1][ADDR+4].xxxx`): cada grupo de cuatro
  caracteres salía con el glifo del primero ("30.0 FPS" → "3333"). Ahora elige la componente con selects. Al arrancar,
  líneas `[dk] FPS counter self-test` (bits de los glifos, offsets de `TextUbo` frente a std140, lectura de prueba).
  El GLSL convertido del juego no usa ese patrón (comprobado en las 7680 conversiones de la cosecha).

**Prueba en hardware para el propietario (P3 parte A)** (ficheros en `build/deko3d-out/` del checkout principal):
1. **Copiar** a `sdmc:/switch/wwhd/`: `wwhd_dk.nro` (hbmenu: "SwitchWakerHD (deko3d debug)", libdeko3dd con
   validación) y `wwhd_dk_release.nro` (hbmenu: "SwitchWakerHD (deko3d)", librería release, para medir). `wwhd.nro` es el GL de
   esta rama, para comparar. `shadercache_dksh.bin` no cambia desde P2 (copiarlo solo si en la SD no está esa versión).
   No hace falta tocar `env.txt`: todo lo nuevo está activo por defecto.
2. **Arrancar `wwhd_dk.nro`** y mirar el **contador FPS**: debe leerse "30.0 FPS" (o el número real), no "3333".
3. **Ir a Outset** (o donde se vieran las texturas mal). Con una textura mal **en pantalla, pulsar a la vez los dos
   sticks (L3 + R3)**. El juego se para un momento en ese frame (copia las imágenes de la GPU) y sigue; los PNG se siguen
   escribiendo en segundo plano unos segundos: **esperar ~30 s** sin salir antes de la siguiente captura o de cerrar. El
   log dice `[dk] capture of frame N: all M files done`. Hacer 2 o 3 capturas en sitios distintos con texturas mal.
4. **Opcional, si hay tiempo:** salir, poner `WWHD_DK_RT_COMPRESSION=0` en `sdmc:/switch/wwhd/env.txt`, repetir una
   captura en el mismo sitio y anotar si la textura sigue mal; luego quitar la línea. Si algo raro aparece con la
   resolución (imagen borrosa, saltos), `WWHD_DYNAMIC_RES=0` la apaga.
5. **Cerrar el juego desde el menú HOME** (así el log se cierra entero) y **montar la SD en el Mac** (hekate UMS). No
   hace falta mandar nada a mano: se leen `sdmc:/switch/wwhd/captures/<frame>/` (`frame_<n>.png` = imagen de TV,
   `frame_<n>_window.png` = pantalla final con el contador, `target_*.png`, `tex_*.png` y `tex_*_upload.png`),
   `sdmc:/switch/wwhd/wwhd.log` y `sdmc:/switch/wwhd/logs/`. Basta con decir en una frase qué se veía mal (qué objeto
   o zona: "la hierba negra", "el mar a cuadros"...), para buscarlo en las capturas.
6. **wwhd_dk_release.nro** después, solo si el debug fue bien: mismo recorrido, para los tiempos (`fps`, `[hitch]`,
   `[dk] GPU passes`, `[dk] internal resolution`).
Si algo se cierra: el log tiene las últimas líneas (`FATAL`, `[dk] deko3d error in ...`, `queue is in an error state`).

## P2: carriles (interfaces en la rama `deko3d`)

Tres ramas (`p2-surf`, `p2-shader`, `p2-draw`) parten de `deko3d` (`git merge deko3d`) y trabajan en paralelo, cada
una en sus ficheros. Las interfaces están en `gfx/deko/dk_surfaces.h`, `dk_shaders.h` y `dk_draw.h`. Integrados
el 2026-10-07 (ver Estado); los contratos siguen valiendo para P3. Diferencias con lo escrito abajo: el worker de uam
hace él mismo `glsl_to_deko`, y la cache local va a `shadercache_dksh_local.bin`.

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
