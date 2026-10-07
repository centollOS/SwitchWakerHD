# Plan: renderer deko3d para SwitchWakerHD

Plan redactado el 2026-10-07 por un agente de planificación (Fable) a partir del código (`dev` 30fd148,
round 36), del prototipo de uam (`~/Documents/uam-proto`) y de los logs de hardware. **[V]** = verificado en
código o logs; **[S]** = supuesto a confirmar. Estado: propuesta, no empezado.

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
- Dispositivo con convenciones Vulkan (`OriginUpperLeft | DepthZeroToOne | YAxisPointsDown`) **[S]**, para portar la
  matemática de `gfx/vulkan/draw.cpp`; verificar en P1/P2.
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
