# VBAP — Plugin de audio espacial para sistema LCR

Plugin de audio (VST3 / AU / Standalone) desarrollado en JUCE y C++ que implementa **VBAP (Vector Base Amplitude Panning)** sobre una configuración mínima funcional de tres altavoces (L, C, R).

Es el componente de software del trabajo de grado **"Evaluación de mejora de la percepción en audio espacial mediante la implementación de la técnica Vector Base Amplitude Panning para un auditorio en la Universidad San Buenaventura sede Bogotá"** (Ingeniería de Sonido, Universidad de San Buenaventura, Bogotá, 2026). El plugin es la herramienta con la que se generaron los estímulos de las pruebas subjetivas de escucha; el objeto de estudio es la evaluación perceptual, no el software en sí mismo.

- **Autores:** Bruno Acero, Fernando Daza, Juan Andrés Trujillo
- **Asesor:** Jonnathan Montenegro

## Alcance

El sistema implementa VBAP con **tres altavoces**, que es la configuración mínima con la que la técnica define un triángulo de reproducción. Está pensado para un recinto pequeño (auditorio 309 "Guillermo de Ockham", 11.7 × 8.99 × 2.4 m, T30mid ≈ 0.6 s) y tiene un **sweet spot limitado**, con fuerte dependencia de la posición del oyente y de la geometría del montaje. No pretende equivalencia con sistemas inmersivos de mayor densidad de canales.

## Geometría implementada

Coordenadas del mundo en metros. El oyente se ubica en (0, 0, 1.20), donde 1.20 m es la altura del oído (`kListenerZ`).

| Altavoz | X | Y | Z | Visto desde el oyente |
|---|---|---|---|---|
| L | −1.40 | 2.42 | 1.30 | azimut −30°, elevación ≈ +2° |
| R | +1.40 | 2.42 | 1.30 | azimut +30°, elevación ≈ +2° |
| C | 0.00 | 2.42 | 2.00 | azimut 0°, elevación ≈ +18° |

Los tres altavoces comparten `Y = 2.42 m`, de modo que el triángulo define un plano perpendicular al eje Y. El **sweet spot** es el punto medio entre L y R: (0, 2.42, 1.30).

## Procesamiento

- **Buses:** entrada mono o estéreo → salida de 3 canales `AudioChannelSet::createLCR()` (L = 0, R = 1, C = 2). Una entrada de N canales se colapsa a mono por promedio antes de espacializar, así que el mono pasa sin cambios y el estéreo no queda +6 dB por encima. El cálculo VBAP es de fuente puntual mono; no hay espacialización independiente por canal de entrada.
- **Ganancias VBAP:** se resuelve `g = A \ unit(srcPos)` con Eigen, donde las columnas de `A` son las posiciones de los altavoces **sin normalizar** (convención de la referencia en MATLAB). Las ganancias negativas se llevan a 0 para evitar inversión de fase cuando la fuente sale del triángulo, y luego se normaliza en energía.
- **Marcos de referencia:** la dirección VBAP se calcula en el marco anclado al oyente (se resta `kListenerZ` en Z). La atenuación por distancia usa en cambio el sweet spot como origen. Esta separación es necesaria: al ser los tres altavoces coplanares con el sweet spot en Y, recentrar la matriz allí la volvería singular.
- **Atenuación por distancia:** curva 1/r reescalada y referida al sweet spot, con `D = ‖fuente − sweetSpot‖`, techo de 0 dB para `D ≤ 1 m` y exactamente −24 dB en `D = 8 m`.
- **Alineación temporal:** delay fijo de **0.73 ms** en el canal central, activable por toggle. Compensa la diferencia de recorrido C vs. L/R (≈ 0.25 m a 343 m/s) para la geometría por defecto.
- **Tiempo real:** `processBlock` no reserva memoria, no toma locks ni hace llamadas al sistema. Los punteros a parámetros se cachean como `std::atomic<float>*` y los buffers se dimensionan en `prepareToPlay`. Las ganancias se suavizan con rampas de 50 ms.

## Parámetros

| ID | Tipo | Rango | Defecto |
|---|---|---|---|
| `sourceX` | float | −1.40 … +1.40 m | 0.00 |
| `sourceY` | float | 2.42 … 6.00 m | 4.00 |
| `sourceZ` | float | 1.20 … 2.00 m | 1.20 |
| `downmix` | bool | — | off |
| `swapLR` | bool | — | off |
| `delayCEnabled` | bool | — | off |

Los rangos de `sourceX` y `sourceZ` están atados a la extensión del triángulo de altavoces (`kSpeakerXAbs`, `kSpeakerZMax`), no al tamaño de la sala: fuera de ese volumen las ganancias VBAP se saturan y el panorama deja de comportarse como se espera.

## Interfaz

Vista 3D de la sala con cámara orbital (1180 × 700 px, tamaño fijo):

- Arrastrar sobre el fondo → rotar la cámara.
- Arrastrar sobre el marcador de la fuente → mover en el plano XY. Con **Shift**, mover en Z.
- Rueda del mouse → zoom. Doble clic → reiniciar la vista.
- Presets de cámara Iso / Top / Front / Side, también en las teclas **1 / 2 / 3 / 4**.

El panel derecho tiene los sliders de posición (rotulados Azimut / Elevación / Distancia, con mapeo lineal y desacoplado a X / Z / Y), los toggles y las barras de ganancia por altavoz en dB, con 0 dB arriba y piso en −24 dB.

## Compilación

**Requisitos:** JUCE con Projucer. Eigen viene vendorizado en `ThirdParty/eigen` y los proyectos lo referencian por ruta relativa, así que no hace falta instalarlo aparte.

**macOS** — abrir `Builds/MacOSX/VBAP.xcodeproj` en Xcode y compilar el esquema "VBAP - All", o desde la terminal:

```bash
cd Builds/MacOSX
xcodebuild -project VBAP.xcodeproj -scheme "VBAP - All" -configuration Release build
```

**Windows** — abrir `Builds/VisualStudio2026/VBAP.sln`, seleccionar Release / x64 y compilar `VBAP_VST3`.

Si se agregan o quitan archivos fuente hay que reexportar con Projucer (`--resave VBAP.jucer`). Lo que está en `Builds/` y `JuceLibraryCode/` es **generado**: se edita el `.jucer`, no esos archivos, y por eso están fuera del control de versiones.

## Estructura

```
Source/
  VBAPProcessor.h      Biblioteca matemática VBAP (header-only, sin estado mutable)
  VBAPProcessor.cpp    Stub de inclusión
  PluginProcessor.*    AudioProcessor de JUCE: buses, DSP, parámetros APVTS
  PluginEditor.*       GUI: vista 3D, cámara orbital, sliders y medidores
ThirdParty/eigen/      Eigen 3 (vendorizado)
VBAP.jucer             Proyecto de Projucer
```

Identificadores del plugin: bundle ID `com.usbbogota.VBAP`, código de fabricante `USBb`, código de plugin `Vbap`, categoría VST3 `Fx|Spatial|Surround`.

## Licencia

Eigen, en `ThirdParty/eigen`, se distribuye bajo sus propias licencias (MPL2 principalmente); ver los archivos `COPYING.*` de esa carpeta. El resto del código es material del trabajo de grado citado arriba.
