// End-to-end check of the lab07 Verilated adapter and the real VGA receiver.
#include <nvboard.h>
#define private public
#include <vga.h>
#undef private
#include "Vnvboard_top.h"
#include <verilated.h>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <vector>

void vga_set_clk_cycle(int cycle);
void init_font(SDL_Renderer *renderer);
void close_font();

static void check(bool condition, const char *label) {
  if (!condition) {
    std::fprintf(stderr, "ERROR: %s\n", label);
    std::exit(1);
  }
}

static void run(Vnvboard_top &dut, VGA &screen, int cycles) {
  while (cycles -- > 0) {
    screen.update_state();
    dut.CLK100MHZ = 0;
    dut.eval();
    dut.CLK100MHZ = 1;
    dut.eval();
  }
}

static void check_image(VGA &screen, const std::vector<unsigned> &rom) {
  check(screen.signal_present, "RTL video acquired");
  for (int y = 0; y < 480; y ++)
    for (int x = 0; x < 640; x ++) {
      unsigned rgb = rom[x * 512 + y];
      unsigned expected = 0xff000000 | (((rgb >> 8) & 15) * 17 << 16) |
                          (((rgb >> 4) & 15) * 17 << 8) | ((rgb & 15) * 17);
      unsigned actual = screen.pixels[y * 640 + x];
      if (actual != expected) {
        std::fprintf(stderr, "ERROR: RTL pixel (%d,%d): %08x != %08x\n",
                     x, y, actual, expected);
        std::exit(1);
      }
    }
}

int main(int argc, char **argv) {
  Verilated::commandArgs(argc, argv);
  std::ifstream memory("rtl/mem/vga_ram.hex");
  std::vector<unsigned> rom;
  unsigned word;
  while (memory >> std::hex >> word) rom.push_back(word);
  check(rom.size() == 640 * 512, "load lab07 reference ROM");
  check(SDL_Init(SDL_INIT_VIDEO) == 0, "SDL init");
  SDL_Window *window = SDL_CreateWindow("RTL test", 0, 0, 1280, 960, SDL_WINDOW_HIDDEN);
  SDL_Renderer *renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
  check(renderer != NULL, "create renderer");
  init_font(renderer);
  {
    Vnvboard_top dut;
    dut.SW = 1;
    for (int i = 0; i < 10; i ++) {
      dut.CLK100MHZ = 0; dut.eval();
      dut.CLK100MHZ = 1; dut.eval();
    }
    for (int pin = 0; pin < NR_PINS; pin ++)
      pin_array[pin] = PinNode{&pin_array[pin].data, 0, 0, 0};
    pin_array[VGA_HSYNC] = PinNode{&dut.VGA_HSYNC, 0, 1, 0};
    pin_array[VGA_VSYNC] = PinNode{&dut.VGA_VSYNC, 0, 1, 0};
    pin_array[VGA_BLANK_N] = PinNode{&dut.VGA_BLANK_N, 0, 1, 0};
    for (int bit = 0; bit < 8; bit ++) {
      pin_array[VGA_R0 + bit] = PinNode{&dut.VGA_R, 0, 8, uint8_t(bit)};
      pin_array[VGA_G0 + bit] = PinNode{&dut.VGA_G, 0, 8, uint8_t(bit)};
      pin_array[VGA_B0 + bit] = PinNode{&dut.VGA_B, 0, 8, uint8_t(bit)};
    }
    vga_set_clk_cycle(4);
    VGA screen(renderer, 1, 0, VGA_TYPE);
    dut.SW = 0;
    run(dut, screen, 3 * 800 * 525 * 4);
    check_image(screen, rom);
    for (int valid : {0, 1}) {
      for (int phase = 0; phase < 4; phase ++) {
        int limit = 800 * 525 * 4;
        while (dut.VGA_BLANK_N != valid && limit -- > 0) run(dut, screen, 1);
        check(limit > 0, "reach reset sampling point");
        run(dut, screen, phase);
        dut.SW = 1;
        run(dut, screen, 10000);
        check(!screen.signal_present, "SW0 high shows no signal");
        for (int pixel = 0; pixel < 640 * 480; pixel ++)
          check(screen.pixels[pixel] == 0xff000000, "SW0 clears stale frame");
        dut.SW = 0;
        run(dut, screen, 3 * 800 * 525 * 4);
        check_image(screen, rom);
      }
    }
    dut.final();
    std::puts("PASS: actual lab07 RTL, all ROM pixels, SW0 reset in blank/active video at four phases, recovery");
  }
  close_font();
  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  SDL_Quit();
}
