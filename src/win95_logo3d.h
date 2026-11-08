#ifndef WIN95_LOGO3D_H
#define WIN95_LOGO3D_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Render real-time 3D Windows 95 flag logo with wave deformation and perspective.
 *
 * @param canvas        Destination 8-bit grayscale canvas
 * @param canvas_w      Canvas width (e.g. 800 or 1648)
 * @param canvas_h      Canvas height (e.g. 600 or 1236)
 * @param stride        Row stride in bytes
 * @param cx            Center X coordinate on screen
 * @param cy            Center Y coordinate on screen
 * @param angle_y       Y-axis rotation angle in radians
 * @param wave_phase    Cloth waving animation phase offset
 */
void win95_logo3d_render(uint8_t *canvas, int canvas_w, int canvas_h, int stride,
                         int cx, int cy, float angle_y, float wave_phase);

#ifdef __cplusplus
}
#endif

#endif /* WIN95_LOGO3D_H */
