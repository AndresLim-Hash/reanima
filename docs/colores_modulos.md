# Identidad cromática por módulo (2026-11-17)

Cada módulo (sección) tiene un color propio, que no se repite dentro de su pestaña. `Section::add` lo aplica solo a sus controles:
- arco y valor de los rotativos;
- botones encendidos;
- flecha y contorno de los menús;
- las curvas de sus dibujos (`moduleColour`).

Los fondos, los textos y los controles apagados siguen neutros.

| Color | Valor | Familia |
|---|---|---|
| Azul | #3fc8ff | fuente o tratamiento principal (motor, filtro, recorte, cadena Principal) |
| Verde | #7be08a | timbre, bucle, macros, matriz, Bus 1 |
| Naranja | #ff9a3c | envolventes (amplitud, Env 2, pitch), Bus 2 |
| Violeta | #b79bff | voz y afinación, LFO 1 |
| Turquesa | #4fd8c4 | Drone Maker, LFO 2 |
| Amarillo | #ffd257 | LFO 3 |
| Rosa | #ff86b4 | expresión MIDI, entrada lateral (SIDECHAIN) |
| Plata | #c3cddd | SALIDA (columna derecha, siempre visible) |

| Pestaña | Módulos |
|---|---|
| SAMPLE | MOTOR azul · DRONE MAKER turquesa · TIMBRE verde · ENVOLVENTE naranja · VOZ·AFINACIÓN violeta |
| EDICIÓN | RECORTE Y NIVEL azul · BUCLE verde · ENVOLVENTE DE PITCH naranja |
| FILTRO·MOD | FILTRO azul · ENV 2 naranja · LFO 1 violeta · LFO 2 turquesa · LFO 3 amarillo · MACROS verde · EXPRESIÓN MIDI rosa |
| MATRIZ | MATRIZ DE MODULACIÓN verde · SIDECHAIN rosa (fuente lateral de la matriz, 23-11) |
| FX | Principal azul · Bus 1 verde · Bus 2 naranja |

El rojo queda reservado para avisos y saturación (medidor).

## Legibilidad al reducir
- **Causa**: el escalado es una transformación vectorial (Segoe UI, Direct2D); no hay mapas de bits estirados. Los textos de 10 px de diseño quedaban en 7 px reales al 70 %. Por debajo de unos 9 px, sin ajuste a la rejilla de píxeles, los trazos caen entre píxeles y se ven borrosos.
- **Solución**: ningún texto baja de 8 px reales (`Theme::onScreen`). Las etiquetas se rehacen al cambiar la escala (`refreshFonts`) y, si no caben, se comprimen hasta un 75 % en horizontal en vez de cortarse.
- **Comprobado**: capturas de las 5 pestañas a 100, 90, 80, 75, 70 y 60 %.
