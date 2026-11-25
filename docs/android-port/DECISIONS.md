# DECISIONS — Decisiones técnicas de arquitectura

1. **Backend gráfico por defecto: GLES3** (no Vulkan). Verificado que el Poco M5s (Mali) soporta GLES 3.2 y Sokol GLES3 ya compila y dibuja.
2. **Mismo proceso para Play Mode en el editor** inicialmente, reutilizando el bucle de `BackendAndroid.cpp`. Se separará a `android:process=":player"` solo si se detecta estado global incompatible.
3. **Exportación de logs y crashes a `Downloads/` vía MediaStore** (`EditorBootLog.java`): sin necesidad de `adb` ni PC, el usuario recibe `Downloads/crash.txt` (cabecera + backtrace) y `Downloads/editor-boot.txt`.
4. **Rama de trabajo unificada**: `arena/593187bf-jasperengine` contiene tanto el port Android (`BackendAndroid.cpp`, `EditorActivity.java`, `EditorBootLog.java`) como los headers verificados (`Android*.h`) y `tools/android/verify.py`.
