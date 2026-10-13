# Matrix Rain (Vulkan 1.1 + Oboe, 100 % C++)

Visualizador musical estilo Matrix para Android. Los glifos son MSDF (`assets/atlas/matrixcode_msdf.png`,
extraído del componente `foo_vis_matrix`); el micrófono (Oboe) modula la velocidad y el brillo de la lluvia.

## Compilar
1. Sube esta carpeta a un repositorio de GitHub (rama `main`).
2. Pestaña **Actions** → *Build APK* → cuando termine, descarga el artifact `MatrixRain-release-apk`.
   También puedes lanzarlo a mano (*Run workflow*) eligiendo `debug` o `release`.

### Firma (opcional)
Sin secrets, el CI genera un keystore temporal (el APK se instala, pero cada build lleva una firma distinta).
Para una firma estable crea estos *Repository secrets*: `KEYSTORE_BASE64` (`base64 -w0 mi.jks`),
`KEYSTORE_PASSWORD`, `KEY_ALIAS`, `KEY_PASSWORD`.

### Local
`gradle :app:assembleDebug` (Gradle 8.9, JDK 17, NDK 27.2.12479018). Requiere minSdk 30 (Android 11+).

## Notas
- Shaders: `app/src/main/shaders/*.vert|frag` los compila AGP a SPIR-V (`assets/shaders/*.spv`).
- Otros atlas del componente (huberfish, gothic, megacity…) pueden usarse cambiando la ruta en
  `vulkan_engine.cpp` y ajustando `kGlyphCount`/tamaño de celda.
- Captura de **micrófono**. Capturar el audio del sistema requiere `MediaProjection` + `AudioPlaybackCapture`
  (API Java), que no se puede hacer solo con Oboe.
- Orientación bloqueada en vertical (el swapchain no implementa pre-rotación).
