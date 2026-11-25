# HANDOFF — Estado actual y siguiente paso

## Estado actual (actualizado tras Fase 0)
- **Rama activa:** `arena/593187bf-jasperengine` (merge completo con `arena/8286230a-jasperengine` + `upstream/main` configurado).
- **Build en curso:** GitHub Actions `Android APK`, run **`45315345205`** (incluye `appendFileHead` en `EditorBootLog.java` para que `Downloads/crash.txt` no se trunque con `/proc/self/maps`).
- **Bugs abiertos prioritarios:**
  1. `SIGABRT` al abrir **Assets** (esperando backtrace real de `Downloads/crash.txt` del run `45315345205`).
  2. **1–2 FPS** en escena (`E/GED: queueBuffer: fence didn't signal in 1000 ms` en Mali).
  3. `Failed to create the Play mode data directory: Read-only file system` (diagnóstico de ruta ya añadido en `editor/Project.cpp:9863`).

## Cómo compilar y probar
- Compilación: `gh workflow run "Android APK" --ref arena/593187bf-jasperengine`
- Precheck local: `tools/android_precheck.py --all`
- Unit tests Android: `tools/android/verify.py --quick`
- En el dispositivo: instalar el APK del run, reproducir el crash de Assets y compartir `Downloads/crash.txt` y `Downloads/editor-boot.txt`.
