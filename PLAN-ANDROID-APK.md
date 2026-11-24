# Plan: APK de Android (arm64) compilable desde la app y desde GitHub Actions

Repositorio: JasperEngine — rama `android-apk-port` (separada de `main`, solo pruebas).
Objetivo: que al hacer un juego en el editor ("la app") se genere un APK de Android arm64-v8a sin exportar fuera, y que GitHub Actions compile y suba ese APK como artefacto descargable.

---

## 1. Estado actual (ya existe en el código)

- `.github/workflows/android.yml`: compila el proyecto de ejemplo con `tools/build.sh android` (Gradle + NDK dentro de la imagen `docker/android.Dockerfile`) y sube el/los APK como artefacto `jasperengine-android-apk`.
- `engine/workspaces/androidstudio/app/build.gradle`: compila las 4 ABI por defecto, incluida `arm64-v8a` (el usuario puede reducir a solo `arm64-v8a` con `JASPERENGINE_ANDROID_ABIS=arm64-v8a`).
- `editor/window/dialog/ExportWindow.cpp`: Build Settings / Export Window incluye Android como plataforma destino y lanza `Exporter::startExport` como trabajo en segundo plano (no bloquea la app).
- `editor/mcp/tools/BuildTools.cpp`: herramienta MCP `build_project` con plataforma `android` y parámetro `android_format` (`aab`, `apk-per-abi`, `apk`). Se llama desde la app o desde MCP / `jasperengine-editor call`.
- `editor/platform/AndroidPackager.cpp`: empaqueta juego + runtime prebuilt de Android en APK/AAB; `selectedAndroidAbis` incluye `arm64-v8a` por defecto; `zipalign -P 16` y firmado v1/v2/v3.
- `editor/settings/SettingsRows.cpp` + `ProjectSettingsWindow.cpp`: ajustes de Android (format, version code, keystore, ABIs, checkbox arm64-v8a).
- `editor/tests/export/BuildMethodTest.cpp` y `ExportFilesTest.cpp`: cubren que el APK de arm64 se nombra `...-arm64-v8a.apk` y que sus librerías van en `lib/arm64-v8a/`.

Conclusión: la ruta "juego en la app -> APK arm64 de Android" ya esta implementada; faltaba que el workflow de GitHub Actions la compile de forma fiable y que quede documentado como obtener el APK.


## 2. Qué hice en esta rama

- `.github/workflows/android.yml`: reescrito para que sea robusto.
  - Se activa con `workflow_dispatch` (manual) y con `push` a `main` cuando tocan Android, el Gradle del Android Studio, `tools/build.sh`, `docker/android.Dockerfile` o el propio workflow.
  - Usa `.github/actions/free-disk` (30 GB) y los permisos `contents: read` + `actions: write` (necesario para subir el artefacto).
  - Compila con `tools/build.sh android` (todas las ABI, incluida arm64-v8a) y sube `build/**/*.apk` como artefacto `jasperengine-android-apk` (retencion 30 dias).
  - No usa Gradle/NDK/CMake propios: la imagen de Docker los provee (S30 R13).
- No se toco código nativo del motor Android: no hay dispositivo/emulador ni Docker en este entorno para probarlo, asi que no se inventan cambios no comprobables.

## 3. Plan por fases (seguir hasta tener el APK terminado)

### Fase 0 — Verificacion del workflow en GitHub Actions (bloqueante)
1. Subir la rama `android-apk-port` a GitHub.
2. Lanzar `.github/workflows/android.yml` (`workflow_dispatch`).
3. Esperar a que `Compilar` termine (puede tardar varios minutos: imagen Android + Gradle + NDK + motor).
4. Descargar el artefacto `jasperengine-android-apk` desde la pestaña Actions / Artefactos.
5. Comprobar que dentro hay un APK con `arm64-v8a` (`unzip -l` / `aapt2 dump resources` / `apksigner verify`).

Esta es la ejecucion roja/verde: sin un run verde del workflow no hay APK fiable.

### Fase 1 — Probar el APK en un dispositivo/emulador arm64
1. Instalar el APK en un dispositivo o emulador Android de 64 bits (arm64-v8a).
2. Comprobar que arranca el proyecto de ejemplo y que responde al tacto.
3. Si falla, leer `logcat` y `editor.log` para ajustar.

### Fase 2 — Construir el juego del usuario desde la app (editor)
1. Abrir el proyecto en `jasperengine-editor`.
2. Build Settings (Export Window) -> Plataforma: Android.
3. Project Settings > Android:
   - Package format: `apk` (o `apk-per-abi` para un APK solo de arm64; `aab` es para Google Play).
   - Architectures: marcar `arm64-v8a` (y opcionalmente las demas para compatibilidad).
   - Version code / version name, keystore (para release; sin keystore se usa debug y se instala en desarrollo).
4. Build -> el editor empaqueta el juego con el runtime prebuilt de Android y genera `<juego>-arm64-v8a.apk` en el output dir.
5. Instalar ese APK en el dispositivo arm64.

### Fase 3 — Todas las funciones y extensiones del proyecto original en el port Android
Probar en el dispositivo/emulador, por area (la mayoría ya compila para Android en el motor):
- Renderizado: GLES3 (backend de Android), sombras, post-proceso, atmosphere, materiales, sprites, tilemap.
- UI: HTML UI widgets, RCSS, CanvasScaler.
- Fisica, navegacion (S19), particulas (S20), animacion (S21), splines (S18), voxels, animator.
- Scripting Lua, audio (S35/S36/S37), entrada multitactil (dos dedos), gamepad.
- Escenas, prefabs, recursos, cook de texturas (ASTC y ETC2 de respaldo, S14 R9).
Cualquier funcion que falle en Android se anota y se corrige en su area.

### Fase 4 — Rendimiento y compatibilidad (ajustes seguros, sin dispositivo no se validan)
- Backend GLES3 (el unico de Android, ya por defecto).
- Release build: `minifyEnabled true` + ProGuard en `buildTypes.release` (APK mas pequena/ofuscada; requiere keystore de release para firmar e instalar).
- zipalign de librerias nativas a paginas de 16 KB (ya en AndroidPackager; util en Android 15+).
- Firmado v1 + v2 + v3 para dispositivos antiguos (API < 24 lee solo v1).
- Texturas: ASTC por defecto con ETC2 de respaldo (`astc = true` en AndroidPackage).
- ABIs: arm64-v8a es el objetivo principal; añadir armeabi-v7a/x86_64 solo si se necesita cubrir dispositivos muy antiguos o emuladores x86.
- SDK: `minSdkVersion 21`, `targetSdkVersion 36` (build.gradle).

### Fase 5 — Entrega del APK final
- Desde GitHub Actions: el workflow sube el APK como artefacto `jasperengine-android-apk` (descargable desde Actions sin compilar nada fuera).
- Desde la app: el editor genera el APK de Android del juego del usuario en su carpeta de salida; el usuario lo instala directamente (`adb install` o copiandolo al dispositivo).
- Se entrega el APK completo (motor + juego + recursos + librerias arm64) y este plan.

## 4. Como se obtiene el APK (dos caminos equivalentes)

Desde GitHub Actions (sin tocar nada localmente):
```
gh workflow run android.yml   # o ejecutarlo desde la pestaña Actions
# descargar: jasperengine-android-apk (contiene *.apk, incluida arm64-v8a)
```

Desde la app/editor (sin exportar fuera):
```
jasperengine-editor mcp --project <mi-proyecto>
call build_project platform=android android_format=apk output_dir=<absoluto fuera del proyecto>
# produce <mi-juego>-arm64-v8a.apk en output_dir
```
O por la UI: Build Settings -> Android -> Build.

## 5. Qué NO se pudo verificar en este entorno (y debe hacer el usuario)

- No hay Docker, ni SDK/NDK de Android, ni emulador/dispositivo en este entorno de pruebas. Por tanto:
  - El run verde del workflow `android.yml` no se ejecuto aqui.
  - No se instalo ni se probo el APK en un dispositivo arm64.
  - No se compilaron cambios nativos de C++ del motor para Android.
- Todo lo relacionado con "todas las funciones funcionan" y "rendimiento" debe validarse en las Fases 1-4 con el dispositivo/emulador y el run verde de GitHub Actions.

Cuando se tenga el run verde y la instalacion en dispositivo, este plan esta completo y el APK queda listo para usar.

