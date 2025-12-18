#include "nes.h"
#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t pack_controller_state(const uint8_t *keys) {
  // NES pad bit order returned by reads: A, B, Select, Start, Up, Down, Left, Right
  uint8_t st = 0;
  if (keys[SDL_SCANCODE_X]) st |= 1 << 0; // A
  if (keys[SDL_SCANCODE_Z]) st |= 1 << 1; // B
  if (keys[SDL_SCANCODE_RSHIFT] || keys[SDL_SCANCODE_LSHIFT]) st |= 1 << 2; // Select
  if (keys[SDL_SCANCODE_RETURN]) st |= 1 << 3; // Start
  if (keys[SDL_SCANCODE_UP]) st |= 1 << 4;
  if (keys[SDL_SCANCODE_DOWN]) st |= 1 << 5;
  if (keys[SDL_SCANCODE_LEFT]) st |= 1 << 6;
  if (keys[SDL_SCANCODE_RIGHT]) st |= 1 << 7;
  return st;
}

static uint32_t fnv1a32(const void *data, size_t n) {
  const uint8_t *p = (const uint8_t *)data;
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < n; i++) {
    h ^= p[i];
    h *= 16777619u;
  }
  return h;
}

int main(int argc, char **argv) {
  bool headless = false;
  int headless_frames = 0;
  bool unthrottled = false;
  bool debug = false;
  uint8_t forced_pad = 0;
  int tap_start_frames = 0;
  int tap_a_frames = 0;
  int tap_b_frames = 0;
  bool detect_freeze = false;
  const char *rom_path = NULL;

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--headless") == 0) {
      if (i + 1 >= argc) {
        fprintf(stderr, "usage: %s --headless <frames> path/to/game.nes\n", argv[0]);
        return 2;
      }
      headless = true;
      headless_frames = atoi(argv[i + 1]);
      if (headless_frames <= 0) headless_frames = 1;
      i++;
      continue;
    }
    if (strcmp(argv[i], "--debug") == 0) { debug = true; continue; }
    if (strcmp(argv[i], "--detect-freeze") == 0) { detect_freeze = true; continue; }
    if (strcmp(argv[i], "--unthrottled") == 0) { unthrottled = true; continue; }
    if (strcmp(argv[i], "--tap-start") == 0) { if (i + 1 < argc) { tap_start_frames = atoi(argv[++i]); } continue; }
    if (strcmp(argv[i], "--tap-a") == 0) { if (i + 1 < argc) { tap_a_frames = atoi(argv[++i]); } continue; }
    if (strcmp(argv[i], "--tap-b") == 0) { if (i + 1 < argc) { tap_b_frames = atoi(argv[++i]); } continue; }
    if (strcmp(argv[i], "--hold-right") == 0) { forced_pad |= 1 << 7; continue; }
    if (strcmp(argv[i], "--hold-left") == 0) { forced_pad |= 1 << 6; continue; }
    if (strcmp(argv[i], "--hold-down") == 0) { forced_pad |= 1 << 5; continue; }
    if (strcmp(argv[i], "--hold-up") == 0) { forced_pad |= 1 << 4; continue; }
    if (strcmp(argv[i], "--hold-start") == 0) { forced_pad |= 1 << 3; continue; }
    if (strcmp(argv[i], "--hold-select") == 0) { forced_pad |= 1 << 2; continue; }
    if (strcmp(argv[i], "--hold-b") == 0) { forced_pad |= 1 << 1; continue; }
    if (strcmp(argv[i], "--hold-a") == 0) { forced_pad |= 1 << 0; continue; }

    if (argv[i][0] != '-') {
      rom_path = argv[i];
      continue;
    }
  }

  if (!rom_path) {
    fprintf(stderr, "usage: %s path/to/game.nes\n", argv[0]);
    fprintf(stderr, "   or: %s [--unthrottled] --headless <frames> path/to/game.nes\n", argv[0]);
    fprintf(stderr, "   or: %s [--unthrottled] path/to/game.nes\n", argv[0]);
    return 2;
  }

  nes_t nes;
  char err[256] = {0};
  if (!nes_load(&nes, rom_path, err, sizeof(err))) {
    fprintf(stderr, "ROM load failed: %s\n", err[0] ? err : "unknown error");
    return 1;
  }

  if (headless) {
    uint32_t h = 0, last_h = 0;
    int same_h = 0;
    int frames_done = 0;
    for (int frame = 0; frame < headless_frames; frame++) {
      uint8_t pad = forced_pad;
      if (tap_start_frames > 0 && frame < tap_start_frames) pad |= (1 << 3);
      if (tap_a_frames > 0 && frame < tap_a_frames) pad |= (1 << 0);
      if (tap_b_frames > 0 && frame < tap_b_frames) pad |= (1 << 1);
      nes.pad1_state = pad;
      if (nes.pad_strobe) nes.pad1_shift = nes.pad1_state;
      (void)nes_run_frame(&nes, 200000);
      h = fnv1a32(nes.ppu.framebuffer, sizeof(nes.ppu.framebuffer));
      if (frame > 0 && h == last_h) same_h++; else same_h = 0;
      last_h = h;
      frames_done = frame + 1;
      if (detect_freeze && same_h > 180) {
        fprintf(stderr, "freeze suspected: framebuffer hash stable for %d frames\n", same_h);
        break;
      }
    }
    printf("frames=%d framebuffer_fnv1a32=%08x\n", frames_done, h);
    if (debug) {
      fprintf(stderr, "cpu_pc=%04x cpu_cycles=%llu ppu_sl=%d ppu_dot=%d mask=%02x status=%02x s0y=%u s0x=%u\n",
              nes.cpu.pc, (unsigned long long)nes.cpu.cycles,
              nes.ppu.scanline, nes.ppu.dot,
              nes.ppu.reg_mask, nes.ppu.reg_status,
              nes.ppu.oam[0], nes.ppu.oam[3]);
      fprintf(stderr, "nmi_count=%llu\n", (unsigned long long)nes.dbg_nmi_count);
      fprintf(stderr, "ppu_w=%d ppu_t=%04x ppu_v=%04x scroll_next=%u,%u render_ctrl_next=%02x\n",
              nes.ppu.w ? 1 : 0, nes.ppu.t, nes.ppu.v,
              nes.ppu.scroll_x_next, nes.ppu.scroll_y_next,
              nes.ppu.render_ctrl_next);
      fprintf(stderr, "fb0=%08x\n", nes.ppu.framebuffer[0]);
    }
    nes_free(&nes);
    return 0;
  }

  SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "nearest");
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS | SDL_INIT_TIMER) != 0) {
    fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
    nes_free(&nes);
    return 1;
  }

  SDL_Window *win = SDL_CreateWindow("nes (mapper0)", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 256 * 3, 240 * 3, SDL_WINDOW_RESIZABLE);
  if (!win) {
    fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
    nes_free(&nes);
    SDL_Quit();
    return 1;
  }
  // Prefer software renderer for compatibility (e.g. VMs).
  SDL_Renderer *ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
  if (!ren) ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
  if (!ren) {
    fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
    nes_free(&nes);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 1;
  }

  SDL_Texture *tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, 256, 240);
  if (!tex) {
    fprintf(stderr, "SDL_CreateTexture failed: %s\n", SDL_GetError());
    nes_free(&nes);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 1;
  }
  SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_NONE);
  SDL_RenderSetLogicalSize(ren, 256, 240);
  SDL_RenderSetIntegerScale(ren, SDL_TRUE);

  // Warm up a couple frames so init code that waits for vblank can run.
  (void)nes_run_frame(&nes, 200000);
  (void)nes_run_frame(&nes, 200000);

  bool running = true;
  const double target_fps = 60.0;
  const uint64_t perf_freq = SDL_GetPerformanceFrequency();
  uint64_t next_frame = SDL_GetPerformanceCounter();

  while (running) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
      if (e.type == SDL_QUIT) running = false;
      if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE) running = false;
      if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_r) nes_reset(&nes);
    }

    const uint8_t *keys = SDL_GetKeyboardState(NULL);
    nes.pad1_state = (uint8_t)(pack_controller_state(keys) | forced_pad);
    if (nes.pad_strobe) nes.pad1_shift = nes.pad1_state;

    // Run until a frame becomes ready
    (void)nes_run_frame(&nes, 200000);

    if (SDL_UpdateTexture(tex, NULL, nes.ppu.framebuffer, 256 * (int)sizeof(uint32_t)) != 0) {
      fprintf(stderr, "SDL_UpdateTexture failed: %s\n", SDL_GetError());
    }
    SDL_RenderClear(ren);
    SDL_RenderCopy(ren, tex, NULL, NULL);
    SDL_RenderPresent(ren);

    if (!unthrottled) {
      uint64_t frame_ticks = (uint64_t)((double)perf_freq / target_fps);
      next_frame += frame_ticks;
      uint64_t now = SDL_GetPerformanceCounter();
      if (now + (perf_freq / 2000) < next_frame) {
        double ms = (double)(next_frame - now) * 1000.0 / (double)perf_freq;
        if (ms > 1.0) SDL_Delay((uint32_t)(ms - 0.5));
        while (SDL_GetPerformanceCounter() < next_frame) {
          // spin a tiny bit to reduce jitter
        }
      } else if (now > next_frame + frame_ticks * 4) {
        // if we're way behind, resync to avoid spiral of death
        next_frame = now;
      }
    }
  }

  nes_free(&nes);
  SDL_DestroyTexture(tex);
  SDL_DestroyRenderer(ren);
  SDL_DestroyWindow(win);
  SDL_Quit();
  return 0;
}
