# PARITY — Matriz de paridad Editor Escritorio vs Android

| Función | Rutas de código | Equivalente táctil | Estado | Bloqueo / Nota | Prueba |
| --- | --- | --- | --- | --- | --- |
| Ventana, GLES3, ciclo de vida | `editor/backend/BackendAndroid.cpp`, `EditorActivity.java` | Pantalla completa inmersiva | **funciona** | Confirmado en Poco M5s (inmersivo OK) | `editor-boot (5)-2.txt` |
| Hub (crear / abrir proyecto) | `editor/App.cpp`, `editor/Project.cpp` | Toque + teclado IME | **funciona** | Arranque bajó a 6.9 s en 2º inicio | `editor-boot (5)-2.txt` |
| Scene / Game View (rendimiento) | `editor/backend/BackendAndroid.cpp`, `editor/App.cpp` | Pellizco zoom, 2 dedos pan/orbit | **roto** | `queueBuffer` 1000 ms/frame en Mali (1–2 FPS) | `editor-boot (6).txt` |
| Assets / Importadores | `editor/window/AssetPanels.cpp`, `AndroidImageLimits.h` | Pulsación larga + menú Insertar | **roto** | `SIGABRT` al abrir Assets; esperando backtrace en `crash.txt` | `editor-boot (4)-2.txt` |
| Play Mode (en editor) | `editor/Project.cpp:9856` | Botón Play táctil | **parcial** | Error `EROFS` en `getUserDataPath()` | Logueo de ruta añadido |
| Audio | `engine/libs/soloud` | N/A | **roto** | Silencio en dispositivo | Pendiente Fase 4.8 |
| Exportar APK en teléfono | `engine/player`, `apksig` | Botón Export + Instalar | **falta** | Planificado en Fase 3 | — |
| Capa Móvil (ayuda ?, barra atajos, paleta, layouts) | `editor/` | — | **falta** | Planificado en Fase 2 | — |
