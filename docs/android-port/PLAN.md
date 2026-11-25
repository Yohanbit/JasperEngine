# PLAN — Port completo del Editor de JasperEngine a Android (arm64-v8a)

## Principio rector
Paridad total con el editor de escritorio (base Linux) + **Capa Móvil** desactivable. Ninguna función se recorta ni se simplifica; lo físicamente imposible en Android va a `KNOWN-LIMITS.md`.

## Fases

- [x] **Fase 0 — Auditoría**
  - [x] Remote `upstream` añadido y divergencia analizada (`git log upstream/main..HEAD`).
  - [x] Inventario de `libs/`, `engine/libs/`, `engine/platform/`, `editor/window/`, `editor/command/`.
  - [x] Confirmado toolkit UI: **Dear ImGui** (`libs/imgui`) + **sokol-gfx** (GLES3 en Android).
  - [x] Documentación inicial en `docs/android-port/` (`PLAN.md`, `PARITY.md`, `DEPENDENCIES.md`, `DECISIONS.md`, `KNOWN-LIMITS.md`, `HANDOFF.md`).

- [ ] **Fase 1 — Módulos + build + entrega + diagnóstico**
  - [x] Unificación de ramas en `arena/593187bf-jasperengine` y build verde en GitHub Actions (`Android APK`, run #63).
  - [x] Exportación automática de `Downloads/crash.txt` (cabecera + backtrace sin truncar) y `Downloads/editor-boot.txt` en `EditorBootLog.java` (run `45315345205`).
  - [ ] Pantalla/botón **Ayuda > Diagnóstico** (`GL_RENDERER`, `GL_VERSION`, Vulkan, RAM, ABI, API level) con botón **Copiar**.
  - [ ] Publicación de pre-release `android-latest` con `JasperEngine-Editor-arm64-v8a.apk` y símbolos `.so`.
  - **Gate:** el APK abre en el Poco M5s y el reporte de diagnóstico se copia.

- [ ] **Fase 2 — Núcleo estable + Capa Móvil base + FPS y Assets**
  - [ ] Obtener backtrace real del `SIGABRT` al abrir **Assets** desde `Downloads/crash.txt` (run `45315345205`) y corregir causa raíz.
  - [ ] Medir y corregir el cuello de botella de **1–2 FPS** (`E/GED: queueBuffer: fence didn't signal in 1000 ms` en Mali) y activar redibujado por eventos (dirty-flag, reposo <= 30 fps, 60 fps al interactuar/Play).
  - [ ] Corregir `D3` (`Failed to create the Play mode data directory: Read-only file system`) usando la ruta logueada.
  - [ ] Ciclo de vida GL completo (pausa/reanudar, superficie perdida/recreada, rotación, multiventana).
  - [ ] Capa móvil base: pulsación larga (clic derecho), pan/zoom táctil, modificadores en pantalla, barra de atajos, paleta de comandos, layouts por orientación, IME con `GameTextInput`.
  - [ ] Crear/abrir/guardar/reabrir proyecto en `/storage/emulated/0/JasperEngine/Projects` (o carpeta de app).
  - **Gate:** monkey de 10 min sin crash; plantilla 2D >= 58 fps medios, p95 < 20 ms.

- [ ] **Fase 3 — Rebanada vertical**
  - [ ] Proyecto 2D -> sprite -> script Lua -> Play -> exportar APK en el teléfono (plantilla precompilada arm64-v8a + zipalign + firma apksig) -> instalar y correr.
  - **Gate:** probado en el Poco M5s.

- [ ] **Fase 4 — Paridad por áreas**
  - [ ] 1) Hierarchy/Inspector/Project/Console/Scene/Game, gizmos, deshacer, prefabs
  - [ ] 2) Importadores y miniaturas (imágenes/GIF/APNG, glTF/OBJ/FBX, audio, video, fuentes)
  - [ ] 3) 2D: tilemaps, luces, normal maps, sombras, Sprite Normal Map
  - [ ] 4) Editor de código, Lua, Console, overlay de depuración
  - [ ] 5) 3D: PBR, luces y sombras, terreno
  - [ ] 6) Animation, Animator, Pose, Avatar, VAT Baker, lattices
  - [ ] 7) Física Box2D/Jolt y Physics Placement
  - [ ] 8) Audio y Audio Mixer
  - [ ] 9) UI toolkit/HTML UI, input actions, controles en pantalla
  - [ ] 10) Partículas, video, fluidos, Texture Synthesis, procmat
  - [ ] 11) Project/Build Settings, shader globals
  - [ ] 12) Servidor MCP y CLI

- [ ] **Fase 5 — App compañera "build environment" (C++, Gradle, AAB)**
  - [ ] Evaluar GABE / proot o respaldo "Compilar en la nube" vía GitHub Actions.

- [ ] **Fase 6 — Rendimiento, térmica, memoria, licencias y README**
