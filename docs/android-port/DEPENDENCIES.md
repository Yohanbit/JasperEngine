# DEPENDENCIES — Inventario de librerías (`libs/` y `engine/libs/`)

| Librería | Ubicación | Uso | Estado en Android |
| --- | --- | --- | --- |
| Dear ImGui | `libs/imgui` | UI y docking del editor | Compila y corre (GLES3) |
| sokol-gfx | `engine/libs/sokol` | Backend gráfico GLES3 | Compila y corre |
| yaml-cpp / json | `libs/yaml-cpp`, `engine/libs/json` | Proyectos y settings | Compila y corre |
| Lua / luabridge3 | `engine/libs/lua`, `engine/libs/luabridge3` | Scripting en editor y runtime | Compila |
| Box2D / JoltPhysics | `engine/libs/box2d`, `engine/libs/joltphysics` | Física 2D y 3D | Compila |
| SoLoud | `engine/libs/soloud` | Audio | Compila (mudo en dispositivo, por revisar) |
| ufbx / tinygltf / tinyobjloader | `libs/ufbx`, `engine/libs/tinygltf` | Importación 3D | Compila |
| stb / tinyexr / miniz / zstd | `engine/libs/stb`, `libs/tinyexr` | Imágenes y compresión | Compila |
| astcenc / basisu | `engine/libs/astcenc`, `engine/libs/basisu` | Texturas móviles ETC2/ASTC | Compila |
| shadercompiler | `engine/libs/shadercompiler` | Compilación de shaders | Compila |
| RmlUi / ThorVG / LunaSVG | `engine/libs/rmlui`, `thorvg`, `lunasvg` | HTML UI y SVG | Compila |
| dav1d / libwebm / opus | `engine/libs/dav1d`, `libwebm`, `opus` | Video y audio WebM | Compila |
| nfd | `libs/nfd` | Diálogos nativos de escritorio | **No aplica** en Android (reemplazar por selector propio + SAF) |
