# Licencia de Reanima y código de terceros

**Reanima** (Blackpanda) se distribuye bajo la **GNU Affero General Public License v3** (texto completo en LICENSE.md).
Quien reciba el plugin tiene derecho a su código fuente completo, y a usarlo, modificarlo y redistribuirlo bajo la misma licencia.
Elegimos esta licencia porque el plugin incluye Rubber Band Library (GPL v2 o posterior) y usa JUCE bajo su licencia abierta (AGPLv3).

| Componente | Uso | Licencia | Dónde |
|---|---|---|---|
| JUCE | marco del plugin | AGPLv3 (licencia abierta de JUCE) | external/JUCE |
| Rubber Band Library 4.0.0 (Breakfast Quay) | LOOP «Rubber Band» (R3, precalculado) | GPL v2 o posterior | external/rubberband-4.0.0 |
| Bungee (Parabola Research), commit 61f52bf | LOOP Complex, colas de Beats | MPL-2.0 (archivos sin modificar; https://github.com/kupix/bungee) | external/bungee-61f52bf |
| Eigen (dentro de Bungee) | álgebra de Bungee | MPL-2.0 | external/bungee-61f52bf/submodules/eigen |
| PFFFT (dentro de Bungee) | FFT de Bungee | tipo BSD (ver su cabecera) | external/bungee-61f52bf/submodules/pffft |
| Signalsmith Stretch 1.4.0 + Signalsmith Linear 0.6.4 | LOOP Complex Pro (formantes), Warp de los motores | MIT | external/signalsmith-* |

Todas son compatibles con la AGPLv3: la GPL v2 «o posterior» se puede usar como GPLv3, MPL-2.0 es compatible con la GPL, y MIT y BSD son permisivas.

**Para cumplir al distribuir el plugin:** acompañarlo del código fuente completo (o de un enlace estable a él), de LICENSE.md y de este archivo.
