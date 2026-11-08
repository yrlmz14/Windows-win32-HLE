#include "win95_logo3d.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int x, y;
} point2d_t;

/* Fast flat-bottom / flat-top triangle rasterizer for 8-bit canvas */
static void fill_triangle(uint8_t *canvas, int canvas_w, int canvas_h, int stride,
                          int x0, int y0, int x1, int y1, int x2, int y2, uint8_t color) {
    if (y0 > y1) { int t = x0; x0 = x1; x1 = t; t = y0; y0 = y1; y1 = t; }
    if (y0 > y2) { int t = x0; x0 = x2; x2 = t; t = y0; y0 = y2; y2 = t; }
    if (y1 > y2) { int t = x1; x1 = x2; x2 = t; t = y1; y1 = y2; y2 = t; }

    if (y0 == y2 || y2 < 0 || y0 >= canvas_h) return;

    int total_h = y2 - y0;
    for (int y = y0; y <= y2; y++) {
        if (y < 0 || y >= canvas_h) continue;
        bool second_half = (y > y1 || y1 == y0);
        int segment_h = second_half ? (y2 - y1) : (y1 - y0);
        if (segment_h == 0) continue;

        float alpha = (float)(y - y0) / total_h;
        float beta  = (float)(y - (second_half ? y1 : y0)) / segment_h;

        int ax = x0 + (int)((x2 - x0) * alpha);
        int bx = second_half ? (x1 + (int)((x2 - x1) * beta)) : (x0 + (int)((x1 - x0) * beta));

        if (ax > bx) { int t = ax; ax = bx; bx = t; }
        if (ax < 0) ax = 0;
        if (bx >= canvas_w) bx = canvas_w - 1;

        uint8_t *row = canvas + (y * stride);
        for (int x = ax; x <= bx; x++) {
            row[x] = color;
        }
    }
}

static void draw_line(uint8_t *canvas, int canvas_w, int canvas_h, int stride,
                      int x0, int y0, int x1, int y1, uint8_t color) {
    int dx = abs(x1 - x0);
    int dy = abs(y1 - y0);
    int sx = (x0 < x1) ? 1 : -1;
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx - dy;

    while (1) {
        if (x0 >= 0 && x0 < canvas_w && y0 >= 0 && y0 < canvas_h) {
            canvas[y0 * stride + x0] = color;
        }
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 < dx)  { err += dx; y0 += sy; }
    }
}

static void render_quad_3d(uint8_t *canvas, int canvas_w, int canvas_h, int stride,
                           point2d_t p0, point2d_t p1, point2d_t p2, point2d_t p3,
                           uint8_t color, uint8_t border_color) {
    fill_triangle(canvas, canvas_w, canvas_h, stride, p0.x, p0.y, p1.x, p1.y, p2.x, p2.y, color);
    fill_triangle(canvas, canvas_w, canvas_h, stride, p0.x, p0.y, p2.x, p2.y, p3.x, p3.y, color);

    draw_line(canvas, canvas_w, canvas_h, stride, p0.x, p0.y, p1.x, p1.y, border_color);
    draw_line(canvas, canvas_w, canvas_h, stride, p1.x, p1.y, p2.x, p2.y, border_color);
    draw_line(canvas, canvas_w, canvas_h, stride, p2.x, p2.y, p3.x, p3.y, border_color);
    draw_line(canvas, canvas_w, canvas_h, stride, p3.x, p3.y, p0.x, p0.y, border_color);
}

void win95_logo3d_render(uint8_t *canvas, int canvas_w, int canvas_h, int stride,
                         int cx, int cy, float angle_y, float wave_phase) {
    if (!canvas || canvas_w <= 0 || canvas_h <= 0) return;

    float pitch = 0.22f; // Slight downward tilt
    float dist  = 3.8f;
    float fov   = 400.0f;

    /* 4 quadrants of the Windows 95 flag: Red, Green, Blue, Yellow */
    float tiles[4][4] = {
        {-1.15f, -1.05f, -0.08f, -0.05f}, // Top-Left (Red)
        { 0.08f, -1.05f,  1.15f, -0.05f}, // Top-Right (Green)
        {-1.15f,  0.08f, -0.08f,  1.08f}, // Bottom-Left (Blue)
        { 0.08f,  0.08f,  1.15f,  1.08f}  // Bottom-Right (Yellow)
    };

    uint8_t base_colors[4] = {
        0x50, // Red (darker gray)
        0x80, // Green (mid gray)
        0x20, // Blue (very dark gray)
        0xE0  // Yellow (bright near white)
    };

    float cos_y = cosf(angle_y);
    float sin_y = sinf(angle_y);
    float cos_p = cosf(pitch);
    float sin_p = sinf(pitch);

    #define GRID_N 4
    for (int t = 0; t < 4; t++) {
        float x_min = tiles[t][0];
        float y_min = tiles[t][1];
        float x_max = tiles[t][2];
        float y_max = tiles[t][3];

        point2d_t proj[GRID_N + 1][GRID_N + 1];

        for (int gy = 0; gy <= GRID_N; gy++) {
            float v = (float)gy / (float)GRID_N;
            float y = y_min + (y_max - y_min) * v;

            for (int gx = 0; gx <= GRID_N; gx++) {
                float u = (float)gx / (float)GRID_N;
                float x = x_min + (x_max - x_min) * u;

                /* Dynamic cloth flag wave deformation */
                float wave = 0.28f * sinf(x * 2.2f + wave_phase) * cosf(y * 1.2f);
                float z = wave;

                /* 3D Rotation */
                float x1 = x * cos_y + z * sin_y;
                float y1 = y;
                float z1 = -x * sin_y + z * cos_y;

                float x2 = x1;
                float y2 = y1 * cos_p - z1 * sin_p;
                float z2 = y1 * sin_p + z1 * cos_p + dist;

                if (z2 < 0.1f) z2 = 0.1f;
                proj[gy][gx].x = cx + (int)(x2 * fov / z2);
                proj[gy][gx].y = cy + (int)(y2 * fov / z2);
            }
        }

        /* Render 4x4 subdivided quad mesh */
        for (int gy = 0; gy < GRID_N; gy++) {
            for (int gx = 0; gx < GRID_N; gx++) {
                point2d_t p0 = proj[gy][gx];
                point2d_t p1 = proj[gy][gx + 1];
                point2d_t p2 = proj[gy + 1][gx + 1];
                point2d_t p3 = proj[gy + 1][gx];

                uint8_t cell_color = base_colors[t];
                int mod = (int)(22.0f * sinf(gx * 0.8f + wave_phase * 0.5f));
                int final_col = (int)cell_color + mod;
                if (final_col < 16) final_col = 16;
                if (final_col > 250) final_col = 250;

                render_quad_3d(canvas, canvas_w, canvas_h, stride, p0, p1, p2, p3, (uint8_t)final_col, 0x10);
            }
        }
    }

    /* 12 Trailing speed dots in 3D perspective behind the flag */
    for (int p = 0; p < 12; p++) {
        float px = -1.30f - (p % 4) * 0.32f;
        float py = -0.80f + (p / 4) * 0.55f;
        float pz = 0.18f * sinf(px * 3.0f + wave_phase);

        float x1 = px * cos_y + pz * sin_y;
        float y1 = py;
        float z1 = -px * sin_y + pz * cos_y;

        float x2 = x1;
        float y2 = y1 * cos_p - z1 * sin_p;
        float z2 = y1 * sin_p + z1 * cos_p + dist;

        if (z2 < 0.1f) z2 = 0.1f;
        int sx = cx + (int)(x2 * fov / z2);
        int sy = cy + (int)(y2 * fov / z2);

        int dot_sz = (int)(15.0f / z2);
        if (dot_sz < 3) dot_sz = 3;

        int half = dot_sz / 2;
        for (int dy = -half; dy <= half; dy++) {
            int py_y = sy + dy;
            if (py_y < 0 || py_y >= canvas_h) continue;
            uint8_t *row = canvas + (py_y * stride);
            for (int dx = -half; dx <= half; dx++) {
                int px_x = sx + dx;
                if (px_x >= 0 && px_x < canvas_w) {
                    row[px_x] = 0x00;
                }
            }
        }
    }
}
