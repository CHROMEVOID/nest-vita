/* Themes change palette, corner language, typography, focus behaviour,
   navigation style and the lofi scene that lives in the background. */
#include "nest.h"

#define A(h, a) (((h) >> 16 & 255) | (((h) >> 8 & 255) << 8) | (((h)&255) << 16) | ((unsigned)(a) << 24))

const Theme THEMES[THEME_COUNT] = {
    {
        .name = "Dusk", .tagline = "Violet evening, soft and quiet",
        .bg = HEX(0x0E0F1A), .bg2 = HEX(0x151629), .surface = HEX(0x1C1D33), .surface2 = HEX(0x26284A),
        .line = HEX(0x2E3058), .text = HEX(0xF2F0FA), .text2 = HEX(0xB9B6D3), .text3 = HEX(0x7D7A9C),
        .accent = HEX(0xB9A6FF), .accent2 = HEX(0xFFB4A2), .on_accent = HEX(0x15122B), .focus = HEX(0xECE6FF),
        .shadow = A(0x05050C, 150), .radius = 14, .radius_sm = 10, .focus_style = 0, .display_font = 0,
        .meta_mono = 0, .light = 0, .nav_style = 0, .scene = "window", .scene_alpha = 0.55f, .grain = 0,
    },
    {
        .name = "Caf\xc3\xa9", .tagline = "Rainy window, warm espresso tones",
        .bg = HEX(0x17110D), .bg2 = HEX(0x1F1712), .surface = HEX(0x2A2019), .surface2 = HEX(0x362920),
        .line = HEX(0x45352A), .text = HEX(0xF6EBDD), .text2 = HEX(0xCDB9A3), .text3 = HEX(0x8E7A66),
        .accent = HEX(0xE9A86A), .accent2 = HEX(0xC9785B), .on_accent = HEX(0x24160D), .focus = HEX(0xF6D7B0),
        .shadow = A(0x080402, 160), .radius = 10, .radius_sm = 8, .focus_style = 2, .display_font = 1,
        .meta_mono = 0, .light = 0, .nav_style = 1, .scene = "cafe", .scene_alpha = 0.6f, .grain = 0,
    },
    {
        .name = "Neon", .tagline = "Late train, crisp edges and glow",
        .bg = HEX(0x07080C), .bg2 = HEX(0x0D0F16), .surface = HEX(0x121521), .surface2 = HEX(0x1A1E2E),
        .line = HEX(0x252A40), .text = HEX(0xEAF6FF), .text2 = HEX(0x9FB3C8), .text3 = HEX(0x5E6E85),
        .accent = HEX(0x3EF0D8), .accent2 = HEX(0xFF4FA3), .on_accent = HEX(0x031614), .focus = HEX(0x3EF0D8),
        .shadow = A(0x000000, 170), .radius = 6, .radius_sm = 4, .focus_style = 1, .display_font = 2,
        .meta_mono = 1, .light = 0, .nav_style = 1, .scene = "city", .scene_alpha = 0.5f, .grain = 1,
    },
    {
        .name = "Moss", .tagline = "Greenhouse calm, round and gentle",
        .bg = HEX(0x0C130F), .bg2 = HEX(0x111B15), .surface = HEX(0x17241C), .surface2 = HEX(0x203227),
        .line = HEX(0x2A4033), .text = HEX(0xEEF3E8), .text2 = HEX(0xB5C4AE), .text3 = HEX(0x74866F),
        .accent = HEX(0xB8D98C), .accent2 = HEX(0xE8C77E), .on_accent = HEX(0x13200F), .focus = HEX(0xDFF0C8),
        .shadow = A(0x020503, 150), .radius = 18, .radius_sm = 12, .focus_style = 0, .display_font = 0,
        .meta_mono = 0, .light = 0, .nav_style = 0, .scene = "plants", .scene_alpha = 0.55f, .grain = 0,
    },
    {
        .name = "Paper", .tagline = "Daylight editorial, ink on cream",
        .bg = HEX(0xF4EFE6), .bg2 = HEX(0xEBE4D7), .surface = HEX(0xFFFDF8), .surface2 = HEX(0xE5DCCB),
        .line = HEX(0xD8CDB8), .text = HEX(0x23201B), .text2 = HEX(0x5E574B), .text3 = HEX(0x958C7C),
        .accent = HEX(0xC2603C), .accent2 = HEX(0x3F6B5E), .on_accent = HEX(0xFFF8F0), .focus = HEX(0x23201B),
        .shadow = A(0x5A4630, 70), .radius = 12, .radius_sm = 8, .focus_style = 2, .display_font = 1,
        .meta_mono = 0, .light = 1, .nav_style = 1, .scene = "lake", .scene_alpha = 0.4f, .grain = 0,
    },
    {
        .name = "Lantern", .tagline = "Midnight ramen stall, lantern glow",
        .bg = HEX(0x0A1014), .bg2 = HEX(0x0F181E), .surface = HEX(0x142229), .surface2 = HEX(0x1C2F38),
        .line = HEX(0x26404B), .text = HEX(0xEAF4F2), .text2 = HEX(0xA3BDBA), .text3 = HEX(0x5F7D7C),
        .accent = HEX(0xFF8A65), .accent2 = HEX(0x7FD8C9), .on_accent = HEX(0x1C0D07), .focus = HEX(0xFFC3A8),
        .shadow = A(0x000305, 160), .radius = 14, .radius_sm = 10, .focus_style = 1, .display_font = 0,
        .meta_mono = 0, .light = 0, .nav_style = 0, .scene = "ramen", .scene_alpha = 0.55f, .grain = 0,
    },
};

const Theme *T = &THEMES[0];

void theme_apply(int idx) {
    if (idx < 0 || idx >= THEME_COUNT) idx = 0;
    T = &THEMES[idx];
    gfx_load_fonts(T->display_font, T->meta_mono);
    vita2d_set_clear_color(T->bg);
}
