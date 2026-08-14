# Instaladores — VBAP v1.0.0

Binarios compilados del plugin, listos para instalar. No hace falta compilar nada para usarlos: basta copiar el archivo correspondiente a la carpeta de plugins del sistema y reiniciar el DAW.

| Plataforma | Formato | Carpeta | Arquitectura |
|---|---|---|---|
| macOS | VST3 | `macOS_AppleSilicon/VBAP.vst3` | arm64 (Apple Silicon) |
| macOS | Audio Unit | `macOS_AppleSilicon/VBAP.component` | arm64 (Apple Silicon) |
| Windows | VST3 | `Windows_x64/VBAP.vst3` | x86-64 |

Versión 1.0.0, bundle ID `com.usbbogota.VBAP`. En macOS y en Windows los `.vst3` y el `.component` son **carpetas** (bundles), no archivos sueltos: hay que copiar la carpeta completa.

## macOS (Apple Silicon)

Copiar cada bundle a su carpeta de plugins:

```bash
cp -R VBAP.vst3      ~/Library/Audio/Plug-Ins/VST3/
cp -R VBAP.component ~/Library/Audio/Plug-Ins/Components/
```

Los binarios están firmados solo *ad-hoc*, sin notarizar, así que al descargarlos desde GitHub macOS los marca en cuarentena y el DAW los rechaza sin dar mucha explicación. Hay que quitar el atributo después de copiarlos:

```bash
xattr -dr com.apple.quarantine ~/Library/Audio/Plug-Ins/VST3/VBAP.vst3
xattr -dr com.apple.quarantine ~/Library/Audio/Plug-Ins/Components/VBAP.component
```

Logic Pro valida el Audio Unit al arrancar; si no aparece, correr el escaneo desde *Logic Pro → Preferencias → Plug-in Manager → Reiniciar y escanear selección*. Reaper y los demás hosts VST3 lo detectan al reescanear la carpeta.

Estos binarios son **solo arm64**. En un Mac Intel no cargan; ahí toca compilar desde el código fuente (ver el README de la raíz).

## Windows (x64)

Copiar la carpeta `VBAP.vst3` completa a la ruta estándar de VST3:

```
C:\Program Files\Common Files\VST3\
```

Queda entonces `C:\Program Files\Common Files\VST3\VBAP.vst3\Contents\x86_64-win\VBAP.vst3`. Requiere permisos de administrador; si no se tienen, sirve cualquier carpeta que el DAW tenga registrada como ruta de VST3.

Necesita el **Visual C++ Redistributable 2015-2022 (x64)** de Microsoft, que en la mayoría de equipos ya está instalado. Si el plugin no aparece tras el escaneo, ese suele ser el motivo.

No hay versión AU para Windows: Audio Unit es un formato exclusivo de macOS.

## Configuración en el DAW

El plugin tiene entrada mono o estéreo y **salida de 3 canales en configuración LCR** (L = canal 1, R = canal 2, C = canal 3). La pista donde se inserte debe estar configurada con al menos 3 canales de salida, o el DAW solo entregará L y R y el canal central se perderá.

El detalle de parámetros, geometría del sistema y procesamiento está en el [README de la raíz del repositorio](../README.md).
