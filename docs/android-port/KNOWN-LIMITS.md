# KNOWN-LIMITS — Limitaciones físicas de Android

1. **Compilación local de scripts C++ / Gradle / NDK dentro del proceso del editor**: Android prohíbe `exec()` de binarios arbitrarios desde el almacenamiento de la app (W^X / SELinux).
   - **Mitigación**: Exportación ligera (Fase 3) con plantilla precompilada `arm64-v8a`; compilación pesada (Fase 5) mediante app compañera ("build environment") o respaldo en la nube vía GitHub Actions.
2. **Diálogos nativos de escritorio (`nfd`)**: no existen en Android. Se reemplazan por selector propio del editor + Storage Access Framework (SAF) como respaldo.
3. **Terminal / VCS / LSP de escritorio**: si dependen de subprocesos externos no disponibles en Android, muestran aviso claro en la UI.
