# Auditoría: JasperEngine en Android

Fecha: 2026-11-23
Rama: `arena/8286230a-jasperengine` (fork `Yohanbit/JasperEngine`)
Commits de este trabajo: `b30e8396`, `b6a1f8d6`, `dbf850e6`, `479f72b9`

---

## 1. El "bloqueo en el logotipo J" no es un bloqueo

**Hallazgo principal, con evidencia.** El motor funciona. Lo que se ve en pantalla no es
una pantalla de carga ni un splash de Android: es **la escena del proyecto de ejemplo
renderizada correctamente** por el motor.

### Evidencia 1: la imagen es un asset del proyecto, no un splash

`engine/project/main.cpp` es todo el juego:

```cpp
JASPERENGINE_INIT void init(){
    image.setAnchorPreset(AnchorPreset::CENTER);
    image.setTexture("jasperengine.png");     // <-- este PNG
    Engine::setScalingMode(Scaling::FITWIDTH);
    Engine::setCanvasSize(1000,480);
    Engine::setScene(&scene);
}
```

`engine/project/assets/jasperengine.png` (413x260) es exactamente el letrero "J /
JASPERENGINE" que aparece en la captura del usuario.

### Evidencia 2: la geometría coincide

Con `Scaling::FITWIDTH` en una pantalla de 1080x2400:

| Cálculo | Valor |
|---|---|
| Imagen dibujada | 1080 x 680 px |
| Centrada verticalmente | y de 860 a 1540 |
| Centro vertical de la pantalla | y = 1200 |
| Centro de la imagen | y = 1200 |

La captura muestra el logo justo en el centro de la pantalla, con el mismo aspecto
(1.588x) que el PNG. Es el render del motor.

### Evidencia 3: el log dice que el motor arrancó bien

Del reporte `jasperengine-boot.txt` del dispositivo (Xiaomi 2207117BPG, Android 13, Mali-G76):

```
D nativeloader : Load .../lib/arm64/libjasperengine-android.so ... : ok
D company.project: JNI_OnLoad success
I GameActivity : Found library libjasperengine-android.so. Loading...
I JasperEngine : [gpu] shader creation: worker
I JasperEngine : Graphics: OpenGL ES 3 / WebGL2; OpenGL ES 3.2 v1.r32p1-01eac0...; Mali-G76
I JasperEngine : Device tier: Mali-G76, rule name (mali g76), tier Low (OpenGL ES 3 / WebGL2)
```

Contexto gráfico GLES 3.2 creado correctamente en Mali-G76. Sin SIGSEGV, sin
`UnsatisfiedLinkError`, sin error de EGL.

### Conclusión

| Lo que se creía | Lo que es |
|---|---|
| "Se queda bloqueado en el splash" | Renderiza la escena del ejemplo |
| "No inicializa el motor" | El motor inicializa y dibuja |
| "Falla por incompatibilidad gráfica" | GLES 3.2 funciona en Mali-G76 |

**El problema real es distinto: el APK compila y empaqueta `engine/project`, que es una
demo de una imagen. No es el editor.**

---

## 2. Qué contiene realmente la APK

`engine/workspaces/androidstudio/app/build.gradle`:

- `sourceSets.main.java.srcDirs = platform/android/java` → solo `MainActivity`,
  `JasperPlatform`, `UserSettings`, `JasperHttp`.
- El nativo compila `engine/CMakeLists.txt` con `APP_NAME jasperengine-android`.
- Assets: `engine/project/assets` + `engine/project/lua`.

`engine/workspaces/androidstudio` **no tiene ningún target de editor**. No hay
`jasperengine-editor` en el APK.

### Qué existe hoy en Android (verificado)

| Componente | Estado |
|---|---|
| Runtime del motor (render, audio, física, Lua, ECS) | ✅ funciona |
| NativeActivity / GameActivity + JNI | ✅ funciona |
| Backend GLES3 (`engine/platform/android`) | ✅ funciona |
| Backend de video (MediaCodec) | ✅ existe |
| HTTP (backend Android) | ✅ existe |
| Empaquetado Gradle, APK/AAB, firma | ✅ funciona |
| **Editor (ImGui)** | ❌ **no existe para Android** |

---

## 3. Por qué el editor no está en Android: análisis del código

El editor **no es multiplataforma**. Está construido sobre **ImGui 1.92.8** con backends
nativos por plataforma, y Android no tiene ninguno implementado.

### `CMakeLists.txt:255-349` — el backend es obligatorio y solo hay tres

```cmake
if(API_BACKEND STREQUAL "linux")
    ...
elseif(API_BACKEND STREQUAL "win")
    ...
elseif(API_BACKEND STREQUAL "mac")
    ...
else()
    message(FATAL_ERROR "Unknown API_BACKEND '${API_BACKEND}' (expected linux, win, or mac)")
endif()
```

No existe `API_BACKEND=android`. Configurar el editor para Android **falla en la
configuración de CMake**, no en la compilación.

### El trabajo pendiente es concreto y medible

| Dato | Valor |
|---|---|
| Archivos del editor | 1,728 `.cpp` + 127 entradas en `editor/window/` |
| Backend Linux ya escrito | `editor/backend/BackendLinux.cpp` = **3,175 líneas** de X11/GLX |
| Backend Windows | `editor/backend/BackendWin.cpp` (Win32 + DWM) |
| Backend macOS | `editor/backend/BackendMac.mm` (AppKit + Metal) |
| Archivos del editor con X11/GLFW/`windows.h` | 51 |
| Contrato a implementar | `editor/Backend.h` (66 líneas, ~15 métodos) |

Un backend Android requiere: `ImGui_ImplAndroid` (existe en `libs/imgui/backends/`),
`ImGui_ImplOpenGL3` (existe), un bucle de frame sobre `NativeEngine`/`GameActivity`,
input táctil → `ImGuiIO`, teclado virtual (`GameActivity_setImeEditorInfo`, ya usado por
el runtime), y las funciones de `Backend.h` que hoy asumen ficheros del sistema.
Además `nfd` (NativeFileDialog) y el empaquetado de proyectos para escritorio no tienen
equivalente Android.

**Es un proyecto de ingeniería serio (semanas, no horas), no un bug de arranque.**

---

## 4. Correcciones ya implementadas

### 4.1 El workflow de CI (commit `b30e8396`)

El workflow original compilaba las 4 ABIs sin caché alguna, con `timeout-minutes: 120`
(el CI del propio proyecto usa 180 para esa misma tarea).

- `ANDROID_ABIS` por defecto `arm64-v8a` (todo teléfono desde 2017); elegible desde el run.
- ccache y Gradle cacheados entre ejecuciones → de ~25 min a **~5 min**.
- Acciones fijadas al SHA completo (obligatorio para `tools/ci/lint.sh`).
- `free-disk` con la necesidad de 65 GB de la lane, `image` action por hash de contenido,
  `tools/ci/logged.sh`, `timeout-minutes: 180`.

### 4.2 Sistema de diagnóstico (commit `b6a1f8d6`)

`JasperBootLog.java` — escribe `Downloads/jasperengine-boot.txt` sin PC, sin ADB y sin
permiso `READ_LOGS`:

- Lee el logcat del propio proceso (un app puede leer el suyo) + identidad del
  dispositivo + excepción fatal si la hubo.
- Escrito a los 20 s, a los 60 s (un cuelgue deja en disco hasta dónde llegó) y en
  `onStop` (cuando el usuario va a buscarlo).
- `MediaStore` en API 29+ (sin permisos), directorio externo propio como respaldo.
- Compartir con pulsación larga (`FileProvider` + share sheet).
- **Solo en builds debug** (`FLAG_DEBUGGABLE`): un export release no lo lleva.
- El fallo de `System.loadLibrary` se captura en vez de lanzarse desde el inicializador
  estático (una clase cuyo inicializador lanza nunca llega a `onCreate`: no se podría
  escribir nada).

**Verificado en el dispositivo del usuario**: el archivo se generó y se pudo leer.

### 4.3 Tier de dispositivo + trazas de arranque (commit `479f72b9`)

- `engine/project/main.cpp` no llamaba `Device::configure()`, así que el tier detectado
  (`Low` en Mali-G76) **se descartaba** (`Device.cpp:192` exige `configured`). El
  generador del editor sí lo escribe (`editor/Generator.cpp:2642`); el proyecto de
  ejemplo, escrito a mano, no. Corregido.
- `Engine::systemStartProject()` registra cada etapa en ms (`lua`, `native plugins`,
  `init()`, `jobs`) y `systemDraw()` registra el primer frame dibujado.

### 4.4 Un error de compilación corregido (commit `dbf850e6`)

`android.os.Process` (importado por `myPid()`) eclipsaba a `java.lang.Process` en
`Process process = new ProcessBuilder(...)`. Solo el build del APK lo detectó: los
`.java` viven fuera del árbol de CMake.

---

## 5. Qué falta, en orden de valor

| # | Objetivo | Esfuerzo real | Estado |
|---|---|---|---|
| 1 | Confirmar en el teléfono que la escena y las trazas nuevas funcionan | minutos | APK compilada, verde |
| 2 | Un proyecto de demo que **no** sea una imagen (3D, física, Lua, UI) | bajo | pendiente |
| 3 | **Backend Android del editor** (`API_BACKEND=android`) | alto (semanas) | no existe |
| 4 | UI táctil del editor (paneles, gestos, teclado virtual) | alto | no existe |
| 5 | Sistema de ficheros de proyectos en Android (SAF) | medio | no existe |

El punto 3 no se puede abordar "de golpe" sin romper lo que ya funciona; y el criterio
del encargo prohíbe explícitamente sacrificar estabilidad por implementar todo a la vez.

---

## 6. Cómo instalar y probar lo de ahora

1. Descargar el artifact `jasperengine-apk-arm64-v8a` de la última ejecución verde de
   **Android APK**.
2. Instalar sobre la V8 existente: mismo `applicationId` (`com.yourcompany.project`) y
   misma clave de depuración, así que **actualiza sin desinstalar ni perder datos**.
3. Abrir, esperar a que aparezca el logo, salir.
4. `Descargas/jasperengine-boot.txt` tendrá el diagnóstico, ahora con:
   - `[start] lua: N ms`
   - `[start] native plugins: N ms`
   - `[start] the project's init(): N ms`
   - `[start] jobs: N ms`
   - `[start] first drawn frame: N ms after the start`
   - `Device tier: ... tier Low` **sin** el sufijo "not applied"
