#include "gb_internal.h"

static uint32_t cgb_color(const uint8_t *palette, unsigned index) {
  uint16_t value = (uint16_t)(palette[index * 2] | palette[index * 2 + 1] << 8);
  unsigned r = value & 31, green = (value >> 5) & 31, b = (value >> 10) & 31;
  return 0xff000000u | ((r * 255 / 31) << 16) | ((green * 255 / 31) << 8) |
         (b * 255 / 31);
}
static uint8_t tile_pixel(const gb_t *g, int tile, unsigned row, unsigned col,
                          unsigned bank) {
  int base = tile >= 384 ? 0x1000 + (tile - 512) * 16 : tile * 16;
  size_t a = (size_t)(base + (int)(row * 2));
  uint8_t lo = g->vram[bank][a & 0x1fff], hi = g->vram[bank][(a + 1) & 0x1fff];
  return (uint8_t)(((hi >> (7 - col)) & 1) * 2 + ((lo >> (7 - col)) & 1));
}
void ppu_line(gb_t *g, unsigned y) {
  static const uint32_t color[] = {0xfff8f8f8, 0xffa8a8a8, 0xff585858,
                                   0xff101010};
  uint8_t lcdc = g->mem[0xff40], scx = g->mem[0xff43], scy = g->mem[0xff42];
  uint8_t wy = g->mem[0xff4a], wx = g->mem[0xff4b];
  unsigned window = (lcdc & 0x20) && y >= wy;
  for (unsigned x = 0; x < 160; x++) {
    unsigned px = x + scx, py = y + scy;
    if (window && x + 7 >= wx) {
      px = x + 7 - wx;
      py = y - wy;
    }
    unsigned map = window ? ((lcdc & 0x40) ? 0x1c00 : 0x1800)
                          : ((lcdc & 8) ? 0x1c00 : 0x1800);
    unsigned tx = (px >> 3) & 31, ty = (py >> 3) & 31;
    uint8_t t = g->vram[0][map + ty * 32 + tx],
            attr =
                g->model == GB_MODEL_CGB ? g->vram[1][map + ty * 32 + tx] : 0;
    uint8_t pal = g->mem[0xff47];
    int tile = (lcdc & 0x10) ? t : (int8_t)t + 512;
    unsigned row = py & 7, col = px & 7;
    if (g->model == GB_MODEL_CGB) {
      if (attr & 0x20)
        col = 7 - col;
      if (attr & 0x40)
        row = 7 - row;
    }
    uint8_t p = (lcdc & 1)
                    ? tile_pixel(g, tile, row, col,
                                 g->model == GB_MODEL_CGB ? (attr >> 3) & 1 : 0)
                    : 0;
    g->bg_line[x] = (uint8_t)(p | ((attr & 0x80) ? 0x80 : 0));
    g->fb[y * 160 + x] = g->model == GB_MODEL_CGB
                             ? cgb_color(g->bg_palette, (attr & 7) * 4 + p)
                             : color[(pal >> (p * 2)) & 3];
  }
  if ((lcdc & 2) && (lcdc & 0x80)) {
    unsigned height = (lcdc & 4) ? 16 : 8, drawn = 0;
    bool used[40] = {false};
    while (drawn < 10) {
      unsigned i = 40, best = 256;
      for (unsigned j = 0; j < 40; j++) {
        int line = (int)y - g->oam[j * 4] + 16;
        if (used[j] || line < 0 || line >= (int)height)
          continue;
        if (i == 40 || (g->opri && g->oam[j * 4 + 1] < best)) {
          i = j;
          best = g->oam[j * 4 + 1];
        }
      }
      if (i == 40)
        break;
      used[i] = true;
      uint8_t sy = g->oam[i * 4], sx = g->oam[i * 4 + 1], t = g->oam[i * 4 + 2],
              a = g->oam[i * 4 + 3];
      int line = (int)y - sy + 16;
      if (line < 0 || line >= (int)height)
        continue;
      drawn++;
      if (a & 0x40)
        line = (int)height - 1 - line;
      if (height == 16)
        t &= 0xfe;
      for (unsigned col = 0; col < 8; col++) {
        unsigned tile_col = a & 0x20 ? 7 - col : col;
        int xx = (int)sx - 8 + (int)col;
        if (xx < 0 || xx >= 160)
          continue;
        uint8_t p = tile_pixel(g, t + (line >= 8), (unsigned)line & 7, tile_col,
                               g->model == GB_MODEL_CGB && (a & 8) ? 1 : 0);
        if (!p || ((a & 0x80) && (g->bg_line[xx] & 0x0f)))
          continue;
        if (g->model == GB_MODEL_CGB && (g->bg_line[xx] & 0x80))
          continue;
        uint8_t pal = a & 0x10 ? g->mem[0xff49] : g->mem[0xff48];
        g->fb[y * 160 + xx] = g->model == GB_MODEL_CGB
                                  ? cgb_color(g->obj_palette, (a & 7) * 4 + p)
                                  : color[(pal >> (p * 2)) & 3];
      }
    }
  }
}
void ppu_stat(gb_t *g) {
  if (!(g->mem[0xff40] & 0x80))
    return;
  uint8_t lyc = g->mem[0xff45], stat = g->mem[0xff41];
  unsigned signal =
      ((g->ppu_mode == 0) && (stat & 8)) ||
      ((g->ppu_mode == 1) && (stat & 16)) ||
      ((g->ppu_mode == 2) && (stat & 32)) ||
      (lyc == g->mem[0xff44] && (stat & 64));
  if (signal && !g->stat_signal)
    g->mem[0xff0f] |= 2;
  g->stat_signal = (uint8_t)signal;
}
unsigned ppu_mode3_length(gb_t *g) {
  uint8_t lcdc = g->mem[0xff40];
  unsigned y = g->mem[0xff44], len = 172 + (g->mem[0xff43] & 7);
  int wx = (int)g->mem[0xff4b] - 7;
  if ((lcdc & 0x20) && y >= g->mem[0xff4a] && wx > 0 && wx < 160)
    len += 6;
  if (!(lcdc & 2))
    return len;
  unsigned height = (lcdc & 4) ? 16 : 8, n = 0;
  uint8_t idx[10];
  for (unsigned j = 0; j < 40 && n < 10; j++) {
    int line = (int)y - g->oam[j * 4] + 16;
    if (line >= 0 && line < (int)height)
      idx[n++] = (uint8_t)j;
  }
  for (unsigned i = 1; i < n; i++) {
    uint8_t t = idx[i], k = (uint8_t)i;
    while (k && g->oam[idx[k - 1] * 4 + 1] > g->oam[t * 4 + 1]) {
      idx[k] = idx[k - 1];
      k--;
    }
    idx[k] = t;
  }
  bool seen = false;
  int seen_key = 0;
  for (unsigned i = 0; i < n; i++) {
    uint8_t sx = g->oam[idx[i] * 4 + 1];
    if (sx >= 168)
      continue;
    int x = (int)sx - 8, win = (lcdc & 0x20) && y >= g->mem[0xff4a] && x >= wx;
    int eff = win ? x - wx : x + g->mem[0xff43],
        tile = eff >= 0 ? eff / 8 : -((-eff + 7) / 8);
    int key = (win << 24) | (tile & 0xffffff), wait = 5 - (eff - tile * 8);
    if (wait < 0)
      wait = 0;
    len += (!seen             ? 3 + (unsigned)wait
            : key != seen_key ? 6 + (unsigned)wait
                              : 6);
    seen = true;
    seen_key = key;
  }
  return len;
}
