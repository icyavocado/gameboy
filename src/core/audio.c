#include "gb_internal.h"

#define AUDIO_RATE 48000u
#define AUDIO_CLOCK 4194304u

uint8_t audio_read(const gb_t *g, uint16_t a) {
  if (a == 0xff26)
    return (uint8_t)(g->mem[a] | 0x70 | (g->audio_enabled[0] ? 1 : 0) |
                     (g->audio_enabled[1] ? 2 : 0) |
                     (g->audio_enabled[2] ? 4 : 0) |
                     (g->audio_enabled[3] ? 8 : 0));
  if (a >= 0xff30 && a <= 0xff3f)
    return g->mem[a];
  if ((a == 0xff76 || a == 0xff77) && g->model == GB_MODEL_CGB) {
    uint8_t pcm[4] = {0};
    for (unsigned channel = 0; channel < 4; channel++) {
      uint16_t base = channel == 0   ? 0xff10
                      : channel == 1 ? 0xff16
                      : channel == 2 ? 0xff1a
                                     : 0xff20;
      if (channel < 2) {
        static const uint8_t duty[] = {0x01, 0x81, 0x87, 0x7e};
        unsigned bit = (g->audio_phase[channel] >> 29) & 7;
        pcm[channel] = (duty[g->mem[base] >> 6] & (1u << bit))
                           ? g->audio_volume[channel]
                           : 0;
      } else if (channel == 2) {
        if (g->audio_wave_delay)
          continue;
        if (!(g->mem[0xff1a] & 0x80))
          continue;
        unsigned index = (g->audio_phase[2] >> 16) & 31;
        uint8_t sample =
            (uint8_t)((g->mem[0xff30 + index / 2] >> (index & 1 ? 0 : 4)) & 15);
        unsigned shift = g->mem[base + 2] >> 5;
        pcm[channel] = shift == 0 ? 0 : (uint8_t)(sample >> (shift - 1));
      } else {
        pcm[channel] = !(g->noise_lfsr & 1) ? g->audio_volume[channel] : 0;
      }
    }
    return (uint8_t)(a == 0xff76 ? pcm[1] << 4 | pcm[0] : pcm[3] << 4 | pcm[2]);
  }
  if (a == 0xff15 || a == 0xff1f || (a >= 0xff27 && a <= 0xff2f))
    return 0xff;
  if (a == 0xff10)
    return (uint8_t)(g->mem[a] | 0x80);
  if (a == 0xff11 || a == 0xff16)
    return (uint8_t)(g->mem[a] | 0x3f);
  if (a == 0xff14 || a == 0xff19 || a == 0xff1e)
    return (uint8_t)(g->mem[a] | 0xb8);
  if (a == 0xff1a)
    return (uint8_t)(g->mem[a] | 0x7f);
  if (a == 0xff1c)
    return (uint8_t)(g->mem[a] | 0x9f);
  if (a == 0xff20)
    return 0xff;
  if (a == 0xff23)
    return (uint8_t)(g->mem[a] | 0xbf);
  return g->mem[a];
}

void audio_trigger(gb_t *g, unsigned channel) {
  static const uint16_t dac_reg[] = {0xff12, 0xff17, 0xff1a, 0xff21};
  static const uint16_t length_reg[] = {0xff11, 0xff16, 0xff1b, 0xff20};
  static const uint16_t volume_reg[] = {0xff12, 0xff17, 0xff1c, 0xff21};
  g->audio_enabled[channel] =
      (uint8_t)((g->mem[dac_reg[channel]] & (channel == 2 ? 0x80 : 0xf8)) != 0);
  g->audio_phase[channel] = 0;
  g->audio_host_phase[channel] = 0;
  if (channel == 2) {
    g->audio_length[2] = (uint16_t)(256 - g->mem[length_reg[2]]);
    g->audio_wave_delay =
        (uint16_t)(2 * (2048 - (((g->mem[0xff1e] & 7) << 8) | g->mem[0xff1d])));
    g->audio_wave_delay = (uint16_t)(g->audio_wave_delay + 6);
  } else {
    g->audio_length[channel] =
        (uint16_t)(64 - (g->mem[length_reg[channel]] & 0x3f));
  }
  if (channel == 2)
    g->audio_volume[2] = (uint8_t)((g->mem[0xff1c] >> 5) & 3);
  if (channel != 2) {
    g->audio_volume[channel] = g->mem[volume_reg[channel]] >> 4;
    g->audio_envelope_timer[channel] = g->mem[volume_reg[channel]] & 7;
    if (!g->audio_envelope_timer[channel])
      g->audio_envelope_timer[channel] = 8;
  }
  if (channel == 0) {
    g->audio_sweep_shadow =
        (uint16_t)((g->mem[0xff14] & 7) << 8 | g->mem[0xff13]);
    g->audio_sweep_timer = g->mem[0xff10] >> 4 & 7;
    if (!g->audio_sweep_timer)
      g->audio_sweep_timer = 8;
    g->audio_sweep_enabled = (uint8_t)((g->mem[0xff10] & 0x77) != 0);
    g->audio_sweep_negate = (uint8_t)((g->mem[0xff10] >> 3) & 1);
  }
  if (channel == 3)
    g->noise_lfsr = 0x7fff;
  g->mem[0xff26] |= (uint8_t)(1u << channel);
}

static int16_t audio_sample(gb_t *g, unsigned channel) {
  static const uint8_t duty[] = {0x01, 0x81, 0x87, 0x7e};
  static const uint16_t volume_reg[] = {0xff12, 0xff17, 0xff1c, 0xff21};
  static const uint16_t freq_low[] = {0xff13, 0xff18, 0xff1d, 0};
  static const uint16_t freq_high[] = {0xff14, 0xff19, 0xff1e, 0};
  static const uint16_t duty_reg[] = {0xff11, 0xff16, 0, 0};
  uint8_t control = g->mem[volume_reg[channel]];
  if (channel == 2) {
    unsigned index = (g->audio_host_phase[2] >> 16) & 31;
    uint8_t volume = (control >> 5) & 3;
    uint8_t sample = (g->mem[0xff30 + index / 2] >> (index & 1 ? 0 : 4)) & 15;
    uint16_t frequency = (uint16_t)((g->mem[freq_high[channel]] & 7) << 8 |
                                    g->mem[freq_low[channel]]);
    if (frequency < 2048)
      g->audio_host_phase[2] +=
          (uint32_t)((((uint64_t)65536u << 21) / (2048u - frequency)) /
                     AUDIO_RATE);
    if (!volume)
      return 0;
    if (volume == 1)
      return (int16_t)(((int)sample - 8) * 128);
    if (volume == 2)
      return (int16_t)(((int)sample - 8) * 64);
    return (int16_t)(((int)sample - 8) * 32);
  }
  if (channel == 3) {
    uint8_t volume = g->audio_volume[3];
    if (!volume)
      return 0;
    return (int16_t)(volume * (!(g->noise_lfsr & 1) ? 512 : -512));
  }
  uint8_t volume = g->audio_volume[channel],
          duty_index = g->mem[duty_reg[channel]] >> 6;
  uint16_t frequency = (uint16_t)((g->mem[freq_high[channel]] & 7) << 8 |
                                  g->mem[freq_low[channel]]);
  uint32_t step =
      frequency < 2048
          ? (uint32_t)((((uint64_t)131072u << 32) / (2048u - frequency)) /
                       AUDIO_RATE)
          : 0;
  unsigned bit = (g->audio_host_phase[channel] >> 29) & 7;
  g->audio_host_phase[channel] += step;
  return (int16_t)((duty[duty_index] & (1u << bit) ? volume : -volume) * 512);
}

void audio_tick(gb_t *g) {
  static const uint16_t freq_low[] = {0xff13, 0xff18, 0xff1d};
  static const uint16_t freq_high[] = {0xff14, 0xff19, 0xff1e};
  for (unsigned channel = 0; channel < 3; channel++) {
    if (!g->audio_enabled[channel])
      continue;
    uint16_t frequency = (uint16_t)((g->mem[freq_high[channel]] & 7) << 8 |
                                    g->mem[freq_low[channel]]);
    unsigned denominator = 2048u - frequency;
    if (!denominator)
      continue;
    if (channel == 2) {
      if (g->audio_wave_delay) {
        if (--g->audio_wave_delay == 0)
          g->audio_phase[channel] = 0x10000;
      } else {
        g->audio_phase[channel] += (uint32_t)(0x8000u / denominator);
      }
    } else {
      g->audio_phase[channel] += (uint32_t)(0x8000000u / denominator);
    }
  }
  if (g->audio_enabled[3]) {
    uint16_t base = 0xff20;
    uint8_t divisor = g->mem[base] & 7, shift = g->mem[base] >> 4;
    unsigned period = (divisor ? divisor * 16u : 8u) << shift;
    g->audio_phase[3]++;
    if (period && g->audio_phase[3] % period == 0) {
      uint16_t bit = (uint16_t)((g->noise_lfsr ^ (g->noise_lfsr >> 1)) & 1);
      g->noise_lfsr = (uint16_t)((g->noise_lfsr >> 1) | (bit << 14));
      if (g->mem[base] & 8)
        g->noise_lfsr = (uint16_t)((g->noise_lfsr & ~(1u << 6)) | (bit << 6));
    }
  }
}

void audio_frame(gb_t *g) {
  unsigned frames;
  int16_t samples[805 * 2];
  g->audio_remainder += 70224u * AUDIO_RATE;
  frames = g->audio_remainder / AUDIO_CLOCK;
  g->audio_remainder %= AUDIO_CLOCK;
  uint8_t routing = g->mem[0xff25];
  uint8_t left = g->mem[0xff24] >> 4, right = g->mem[0xff24] & 7;
  for (unsigned i = 0; i < frames; i++) {
    int16_t value[4];
    for (unsigned channel = 0; channel < 4; channel++)
      value[channel] = g->audio_enabled[channel] ? audio_sample(g, channel) : 0;
    samples[i * 2] = 0;
    samples[i * 2 + 1] = 0;
    for (unsigned channel = 0; channel < 4; channel++) {
      if (routing & (uint8_t)(1u << (channel + 4)))
        samples[i * 2] += value[channel];
      if (routing & (uint8_t)(1u << channel))
        samples[i * 2 + 1] += value[channel];
    }
    int left_sample = samples[i * 2] * left / 8;
    int right_sample = samples[i * 2 + 1] * right / 8;
    g->audio_filter[0] += (left_sample - g->audio_filter[0]) / 4;
    g->audio_filter[1] += (right_sample - g->audio_filter[1]) / 4;
    left_sample = g->audio_filter[0];
    right_sample = g->audio_filter[1];
    int hp_left =
        left_sample - g->audio_hp_x[0] + (int)(g->audio_hp[0] * 1023 / 1024);
    int hp_right =
        right_sample - g->audio_hp_x[1] + (int)(g->audio_hp[1] * 1023 / 1024);
    g->audio_hp_x[0] = left_sample;
    g->audio_hp_x[1] = right_sample;
    g->audio_hp[0] = hp_left;
    g->audio_hp[1] = hp_right;
    left_sample = hp_left;
    right_sample = hp_right;
    if (left_sample > 32767)
      left_sample = 32767;
    if (left_sample < -32768)
      left_sample = -32768;
    if (right_sample > 32767)
      right_sample = 32767;
    if (right_sample < -32768)
      right_sample = -32768;
    samples[i * 2] = (int16_t)left_sample;
    samples[i * 2 + 1] = (int16_t)right_sample;
  }
  if (g->audio && (g->mem[0xff26] & 0x80))
    g->audio(g->audio_user, samples, frames);
}

static int audio_sweep_next(gb_t *g) {
  int delta = g->audio_sweep_shadow >> (g->mem[0xff10] & 7);
  return g->audio_sweep_negate ? (int)g->audio_sweep_shadow - delta
                               : (int)g->audio_sweep_shadow + delta;
}

static void audio_sweep(gb_t *g) {
  uint8_t shift = g->mem[0xff10] & 7;
  int next = audio_sweep_next(g);
  if (next > 2047 || next < 0) {
    g->audio_enabled[0] = 0;
    g->mem[0xff26] &= (uint8_t)~1;
    return;
  }
  if (!shift)
    return;
  g->audio_sweep_shadow = (uint16_t)next;
  g->mem[0xff13] = (uint8_t)next;
  g->mem[0xff14] = (uint8_t)((g->mem[0xff14] & 0xf8) | (next >> 8));
  next = audio_sweep_next(g);
  if (next > 2047 || next < 0) {
    g->audio_enabled[0] = 0;
    g->mem[0xff26] &= (uint8_t)~1;
  }
}

void audio_sequence(gb_t *g) {
  static const uint16_t length_reg[] = {0xff11, 0xff16, 0xff1b, 0xff20};
  unsigned step = g->audio_seq_step;
  if (!(step & 1)) {
    for (unsigned i = 0; i < 4; i++) {
      if (g->audio_length[i] && !(g->mem[length_reg[i] + 3] & 0x40))
        continue;
      if (g->audio_length[i] && --g->audio_length[i] == 0) {
        g->audio_enabled[i] = 0;
        g->mem[0xff26] &= (uint8_t)~(1u << i);
      }
    }
  }
  if (step == 2 || step == 6) {
    if (g->audio_sweep_timer && --g->audio_sweep_timer == 0) {
      if (g->audio_sweep_enabled && ((g->mem[0xff10] >> 4) & 7))
        audio_sweep(g);
      g->audio_sweep_timer = (g->mem[0xff10] >> 4) & 7;
      if (!g->audio_sweep_timer)
        g->audio_sweep_timer = 8;
    }
  }
  if (step == 7) {
    for (unsigned i = 0; i < 4; i++) {
      uint16_t base = i == 0   ? 0xff10
                      : i == 1 ? 0xff16
                      : i == 2 ? 0xff1a
                               : 0xff20;
      uint8_t control = g->mem[base + (i == 3 ? 1 : 2)];
      uint8_t timer = g->audio_envelope_timer[i];
      uint8_t period = (uint8_t)(control & 7);
      if (timer && --timer == 0) {
        uint8_t volume = g->audio_volume[i];
        if (period) {
          if (control & 8) {
            if (volume < 15)
              volume++;
          } else if (volume)
            volume--;
        }
        g->audio_volume[i] = volume;
        timer = period ? period : 8;
      }
      g->audio_envelope_timer[i] = timer;
    }
  }
  g->audio_seq_step = (uint8_t)((step + 1) & 7);
}
