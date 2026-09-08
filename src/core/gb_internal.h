#ifndef GB_INTERNAL_H
#define GB_INTERNAL_H

#include "gb.h"

#define MAX_BREAKPOINTS 16u

struct gb {
  uint8_t *rom, *ram, *boot_rom, mem[65536];
  uint8_t vram[2][0x2000], wram[8][0x1000], oam[0xa0];
  uint8_t bg_palette[64], obj_palette[64];
  size_t rom_size, ram_size;
  uint32_t fb[160 * 144];
  uint8_t bg_line[160];
  uint16_t af, bc, de, hl, sp, pc, t_clock, divider;
  uint8_t ime, ei_delay, halted, halt_bug, input, div, mbc, battery, ram_bank,
      ram_enable, upper, mode, ppu_mode, stat_signal, stat_hblank_pend,
      stat_hblank_dly, ppu_first_line, ppu_enable_line, dma_page, dma_index,
      dma_active, dma_copy, timer_signal, rtc_select, rtc_latched_valid, rtc[5],
      rtc_latched[5], vbk, svbk, bg_palette_index, obj_palette_index, key1,
      opri, ir, serial_active, boot_enabled, double_speed;
  uint16_t rom_bank;
  uint16_t hdma_source, hdma_dest;
  uint8_t hdma5;
  uint16_t serial_cycles;
  uint16_t timer_due;
  uint16_t dma_busy_start, dma_busy_end, dma_next;
  uint16_t ppu_mode3;
  uint16_t ppu_boot;
  unsigned ppu_cycles;
  uint32_t rtc_cycles;
  uint32_t audio_remainder;
  uint32_t audio_phase[4];
  uint32_t audio_host_phase[4];
  int32_t audio_filter[2];
  int32_t audio_hp[2];
  int32_t audio_hp_x[2];
  uint16_t noise_lfsr;
  uint16_t audio_seq_cycles, audio_sweep_shadow, audio_wave_delay;
  uint16_t audio_length[4];
  uint8_t audio_seq_step, audio_volume[4], audio_envelope_timer[4],
      audio_sweep_timer, audio_sweep_enabled, audio_sweep_negate;
  uint8_t audio_enabled[4];
  gb_model_t model;
  uint8_t model_forced;
  gb_serial_cb serial;
  void *serial_user;
  struct gb *serial_peer;
  gb_audio_cb audio;
  void *audio_user;
  gb_bp_t breakpoints[MAX_BREAKPOINTS];
  uint8_t breakpoint_used[MAX_BREAKPOINTS], debug_enabled, watch_hit,
      debug_fetch, debug_pc_hit;
  unsigned instruction_cycles;
};

uint8_t audio_read(const gb_t *, uint16_t);
void audio_trigger(gb_t *, unsigned);
void audio_tick(gb_t *);
void audio_frame(gb_t *);
void audio_sequence(gb_t *);
void ppu_line(gb_t *, unsigned);
void ppu_stat(gb_t *);
unsigned ppu_mode3_length(gb_t *);

#endif
