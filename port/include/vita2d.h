/* Nest port: libvita2d's interface, drawn with OpenGL ES 2 (port/gl.c). */
#ifndef VITA2D_H
#define VITA2D_H

#include "psp2_all.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RGBA8(r, g, b, a) ((((a)&0xFF) << 24) | (((b)&0xFF) << 16) | (((g)&0xFF) << 8) | (((r)&0xFF) << 0))

typedef struct vita2d_color_vertex {
    float x, y, z;
    unsigned int color;
} vita2d_color_vertex;

typedef struct vita2d_texture_vertex {
    float x, y, z;
    float u, v;
} vita2d_texture_vertex;

typedef struct vita2d_texture {
    SceGxmTexture gxm_tex;  /* width/height/format; video frames put their pixels here */
    unsigned int gl;        /* GL texture name, 0 until first drawn */
    unsigned int fbo;       /* render targets */
    unsigned char *pixels;  /* CPU copy: RGBA, or 8-bit indices for palette textures */
    unsigned char *palette; /* 256 x RGBA for palette textures */
    int dirty;              /* pixels changed since the last upload */
    int linear;             /* filtering */
    int is_rt;
    unsigned int uploaded_serial;
} vita2d_texture;

typedef struct vita2d_font vita2d_font;

int vita2d_init(void);
int vita2d_init_advanced(unsigned int temp_pool_size);
int vita2d_init_advanced_with_msaa(unsigned int temp_pool_size, SceGxmMultisampleMode msaa);
void vita2d_wait_rendering_done(void);
int vita2d_fini(void);
void vita2d_clear_screen(void);
void vita2d_swap_buffers(void);
void vita2d_start_drawing(void);
void vita2d_start_drawing_advanced(vita2d_texture *target, unsigned int flags);
void vita2d_end_drawing(void);
int vita2d_common_dialog_update(void);
void vita2d_set_clear_color(unsigned int color);
void vita2d_set_vblank_wait(int enable);
void vita2d_enable_clipping(void);
void vita2d_disable_clipping(void);
void vita2d_set_clip_rectangle(int x_min, int y_min, int x_max, int y_max);
void *vita2d_pool_memalign(unsigned int size, unsigned int alignment);
void *vita2d_pool_malloc(unsigned int size);

void vita2d_draw_rectangle(float x, float y, float w, float h, unsigned int color);
void vita2d_draw_array(SceGxmPrimitiveType mode, const vita2d_color_vertex *vertices, size_t count);
void vita2d_draw_array_textured(const vita2d_texture *texture, SceGxmPrimitiveType mode, const vita2d_texture_vertex *vertices,
                                size_t count, unsigned int color);

void vita2d_texture_set_alloc_memblock_type(SceKernelMemBlockType type);
vita2d_texture *vita2d_create_empty_texture(unsigned int w, unsigned int h);
vita2d_texture *vita2d_create_empty_texture_format(unsigned int w, unsigned int h, SceGxmTextureFormat format);
vita2d_texture *vita2d_create_empty_texture_rendertarget(unsigned int w, unsigned int h, SceGxmTextureFormat format);
void vita2d_free_texture(vita2d_texture *texture);
unsigned int vita2d_texture_get_width(const vita2d_texture *texture);
unsigned int vita2d_texture_get_height(const vita2d_texture *texture);
unsigned int vita2d_texture_get_stride(const vita2d_texture *texture);
void *vita2d_texture_get_datap(const vita2d_texture *texture);
void *vita2d_texture_get_palette(const vita2d_texture *texture);
void vita2d_texture_set_filters(vita2d_texture *texture, SceGxmTextureFilter min_filter, SceGxmTextureFilter mag_filter);

void vita2d_draw_texture(const vita2d_texture *texture, float x, float y);
void vita2d_draw_texture_scale(const vita2d_texture *texture, float x, float y, float x_scale, float y_scale);
void vita2d_draw_texture_tint_scale(const vita2d_texture *texture, float x, float y, float x_scale, float y_scale, unsigned int color);
void vita2d_draw_texture_tint_part_scale(const vita2d_texture *texture, float x, float y, float tex_x, float tex_y, float tex_w,
                                         float tex_h, float x_scale, float y_scale, unsigned int color);

vita2d_texture *vita2d_load_JPEG_file(const char *filename);
vita2d_texture *vita2d_load_PNG_file(const char *filename);

vita2d_font *vita2d_load_font_mem(const void *buffer, unsigned int size);
void vita2d_free_font(vita2d_font *font);
int vita2d_font_draw_text(vita2d_font *font, int x, int y, unsigned int color, unsigned int size, const char *text);
int vita2d_font_text_width(vita2d_font *font, unsigned int size, const char *text);
int vita2d_font_text_height(vita2d_font *font, unsigned int size, const char *text);

#ifdef __cplusplus
}
#endif
#endif
