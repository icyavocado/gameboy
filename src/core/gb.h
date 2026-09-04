#ifndef GB_H
#define GB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct gb gb_t;
typedef enum { GB_MODEL_AUTO, GB_MODEL_DMG, GB_MODEL_CGB } gb_model_t;
typedef void (*gb_audio_cb)(void *user, const int16_t *stereo, size_t frames);
typedef void (*gb_serial_cb)(void *user, uint8_t out, uint8_t *in);
typedef struct { uint16_t af, bc, de, hl, sp, pc; bool ime, halted; } gb_regs_t;
typedef enum { GB_BP_PC, GB_BP_READ, GB_BP_WRITE } gb_bp_kind_t;
typedef struct { gb_bp_kind_t kind; uint16_t addr; } gb_bp_t;

gb_t *gb_create(void);
void gb_destroy(gb_t *gb);
int gb_load_rom(gb_t *gb, const uint8_t *rom, size_t size);
int gb_load_boot_rom(gb_t *gb, const uint8_t *rom, size_t size);
size_t gb_save_ram_size(const gb_t *gb);
size_t gb_save_ram(const gb_t *gb, uint8_t *out);
int gb_load_ram(gb_t *gb, const uint8_t *data, size_t size);
size_t gb_save_state_size(const gb_t *gb);
size_t gb_save_state(const gb_t *gb, uint8_t *out);
int gb_load_state(gb_t *gb, const uint8_t *data, size_t size);
void gb_set_model(gb_t *gb, gb_model_t model);
void gb_reset(gb_t *gb);
void gb_run_frame(gb_t *gb);
const uint32_t *gb_framebuffer(const gb_t *gb);
void gb_set_input(gb_t *gb, uint8_t buttons);
void gb_set_audio_callback(gb_t *gb, gb_audio_cb callback, void *user);
void gb_set_serial_callback(gb_t *gb, gb_serial_cb callback, void *user);
uint8_t gb_dbg_read(const gb_t *gb, uint16_t address);
void gb_dbg_write(gb_t *gb, uint16_t address, uint8_t value);
void gb_dbg_regs(const gb_t *gb, gb_regs_t *out);
int gb_dbg_step(gb_t *gb);
void gb_dbg_enable(gb_t *gb, bool enabled);
int gb_dbg_run_until_break(gb_t *gb);
int gb_dbg_add_bp(gb_t *gb, gb_bp_t breakpoint);
void gb_dbg_del_bp(gb_t *gb, int id);
int gb_dbg_disasm(const gb_t *gb, uint16_t address, char *buf, size_t size);

#endif
