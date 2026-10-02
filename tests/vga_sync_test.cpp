#include <nvboard.h>
#define private public
#include <vga.h>
#undef private
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

void vga_set_clk_cycle(int cycle);
void init_font(SDL_Renderer *renderer);
void close_font();

static uint8_t red, green, blue;
static uint64_t packed_rgb;
static int ratio = 4, source_x = 1, source_y = 1;
static bool blank_hole = false, force_black = false;
static bool missing_hsync = false, missing_vsync = false;
static int sync_width = 96, sync_height = 2, line_width = 800;

static void check(bool condition, const char *label) {
  if (!condition) {
    std::fprintf(stderr, "ERROR: %s\n", label);
    std::exit(1);
  }
}

static void bind_pins(int binding = 0, bool blank_bound = true) {
  for (int pin = 0; pin < NR_PINS; pin ++) {
    pin_array[pin] = PinNode{&pin_array[pin].data, 0, 0, 0};
  }
  for (int pin : {VGA_HSYNC, VGA_VSYNC, VGA_BLANK_N}) {
    pin_array[pin].vector_len = 1;
    pin_array[pin].data = pin != VGA_BLANK_N;
  }
  if (!blank_bound) pin_array[VGA_BLANK_N].vector_len = 0;
  for (int bit = 0; bit < 8; bit ++) {
    for (int channel = 0; channel < 3; channel ++) {
      int pin = (channel == 0 ? VGA_R0 : channel == 1 ? VGA_G0 : VGA_B0) + bit;
      uint8_t *value = channel == 0 ? &red : channel == 1 ? &green : &blue;
      if (binding == 0 || (binding == 2 && channel == 0)) {
        pin_array[pin].ptr = value;
        pin_array[pin].vector_len = 8;
        pin_array[pin].bit_offset = bit;
      } else if (binding == 1 || (binding == 2 && channel == 1)) {
        pin_array[pin].vector_len = 1;
      } else {
        pin_array[pin].ptr = &packed_rgb;
        pin_array[pin].vector_len = 24;
        pin_array[pin].bit_offset = (2 - channel) * 8 + bit;
      }
    }
  }
  source_x = source_y = 1;
  blank_hole = force_black = missing_hsync = missing_vsync = false;
  sync_width = 96;
  sync_height = 2;
  line_width = 800;
}

static void drive_pixel(VGA &vga) {
  pin_array[VGA_HSYNC].data = missing_hsync || source_x > sync_width;
  pin_array[VGA_VSYNC].data = missing_vsync || source_y > sync_height;
  bool active = source_x > 144 && source_x <= 784 && source_y > 35 && source_y <= 515;
  bool hole = blank_hole && source_x >= 245 && source_x <= 254 && source_y == 136;
  pin_array[VGA_BLANK_N].data = active && !hole && !force_black;
  red = (source_x - 145) & 255;
  green = (source_y - 36) & 255;
  blue = 0x5a;
  packed_rgb = (uint64_t(red) << 16) | (uint64_t(green) << 8) | blue;
  for (int bit = 0; bit < 8; bit ++) {
    pin_array[VGA_R0 + bit].data = (red >> bit) & 1;
    pin_array[VGA_G0 + bit].data = (green >> bit) & 1;
    pin_array[VGA_B0 + bit].data = (blue >> bit) & 1;
  }
  for (int cycle = 0; cycle < ratio; cycle ++) vga.update_state();
  if (++source_x > line_width) {
    source_x = 1;
    if (++source_y > 525) source_y = 1;
  }
}

static void run_pixels(VGA &vga, int count) {
  while (count -- > 0) drive_pixel(vga);
}

static void recover(VGA &vga) {
  source_x = source_y = 1;
  run_pixels(vga, 3 * 800 * 525);
  check(vga.signal_present, "reacquire a complete frame");
}

static void check_image(VGA &vga) {
  check(vga.signal_present, "valid signal");
  for (int y = 0; y < 480; y ++)
    for (int x = 0; x < 640; x ++) {
      bool hole = blank_hole && y == 100 && x >= 100 && x < 110;
      uint32_t expected = 0xff000000 |
        (force_black || hole ? 0 : ((x & 255) << 16) | ((y & 255) << 8) | 0x5a);
      if (vga.pixels[y * 640 + x] != expected) {
        std::fprintf(stderr, "ERROR: ratio=%d pixel (%d,%d): %08x != %08x\n",
                     ratio, x, y, vga.pixels[y * 640 + x], expected);
        std::exit(1);
      }
    }
}

static void check_no_signal(VGA &vga) {
  check(!vga.signal_present, "no signal after missing sync");
  for (int i = 0; i < 640 * 480; i ++)
    check(vga.pixels[i] == 0xff000000, "old image cleared on signal loss");
}

static void save_screen(SDL_Renderer *renderer, const char *path) {
  SDL_Surface *surface = SDL_CreateRGBSurfaceWithFormat(0, 1280, 960, 32, SDL_PIXELFORMAT_ARGB8888);
  check(surface != NULL, "create screenshot");
  check(SDL_RenderReadPixels(renderer, NULL, SDL_PIXELFORMAT_ARGB8888,
                            surface->pixels, surface->pitch) == 0, "read rendered screen");
  check(SDL_SaveBMP(surface, path) == 0, "save rendered screen");
  SDL_FreeSurface(surface);
}

int main() {
  check(SDL_Init(SDL_INIT_VIDEO) == 0, "SDL initialization");
  SDL_Window *window = SDL_CreateWindow("VGA regression", 0, 0, 1280, 960, SDL_WINDOW_HIDDEN);
  SDL_Renderer *renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
  check(renderer != NULL, "software renderer");
  init_font(renderer);

  for (int cycles : {0, -1}) {
    bool rejected = false;
    try { vga_set_clk_cycle(cycles); }
    catch (const std::invalid_argument &) { rejected = true; }
    check(rejected, "invalid sampling ratio rejected");
  }

  for (int cycles : {1, 2, 4, 8}) {
    ratio = cycles;
    vga_set_clk_cycle(ratio);
    bind_pins();
    VGA vga(renderer, 1, 0, VGA_TYPE);
    check_no_signal(vga);
    recover(vga);
    check_image(vga);
    // A black video frame still has sync; it must not be called signal loss.
    force_black = true;
    run_pixels(vga, 2 * 800 * 525);
    check_image(vga);
    force_black = false;
    blank_hole = true;
    run_pixels(vga, 2 * 800 * 525);
    check_image(vga);
    blank_hole = false;
    recover(vga);
    check_image(vga);
    if (ratio == 4) save_screen(renderer, "/tmp/nvboard-vga-valid.bmp");

    for (int row : {36, 100, 515}) {
      source_x = source_y = 1;
      run_pixels(vga, (row - 1) * 800 + 300);
      uint32_t displayed = vga.pixels[0];
      // Abrupt reset: frozen pins, either valid level, arbitrary phase.
      for (int cycle = 0; cycle < ratio; cycle ++) {
        for (int n = 0; n < 100; n ++) vga.update_state();
        check(vga.pixels[0] == displayed, "partial frame never published");
      }
      for (int n = 0; n <= 2 * 800 * ratio; n ++) vga.update_state();
      check_no_signal(vga);
      if (ratio == 4 && row == 100) save_screen(renderer, "/tmp/nvboard-vga-no-signal.bmp");
      recover(vga);
      check_image(vga);
    }

    missing_hsync = true;
    run_pixels(vga, 800 * 525);
    check_no_signal(vga);
    missing_hsync = false;
    recover(vga);
    check_image(vga);
    missing_vsync = true;
    run_pixels(vga, 3 * 800 * 525);
    check_no_signal(vga);
    missing_vsync = false;
    recover(vga);
    check_image(vga);
    sync_width = 95;
    run_pixels(vga, 2 * 800 * 525);
    check_no_signal(vga);
    sync_width = 96;
    recover(vga);
    check_image(vga);
    sync_height = 1;
    run_pixels(vga, 2 * 800 * 525);
    check_no_signal(vga);
    sync_height = 2;
    recover(vga);
    check_image(vga);
    line_width = 799;
    run_pixels(vga, 2 * 800 * 525);
    check_no_signal(vga);
    line_width = 800;
    recover(vga);
    check_image(vga);
    std::printf("PASS: ratio %d, complete/black/blanked frames, interrupted scan, missing/invalid sync, recovery\n", ratio);
  }

  ratio = 4;
  vga_set_clk_cycle(ratio);
  for (int binding : {1, 2, 3}) {
    bind_pins(binding);
    VGA vga(renderer, 1, 0, VGA_TYPE);
    recover(vga);
    check_image(vga);
    std::printf("PASS: RGB binding %d (single-bit/mixed/packed)\n", binding);
  }
  bind_pins(0, false);
  {
    VGA vga(renderer, 1, 0, VGA_TYPE);
    recover(vga);
    check_image(vga);
    std::puts("PASS: unbound blank input uses sync-derived active area");
  }
  bind_pins();
  for (int pin = VGA_VSYNC; pin <= VGA_B7; pin ++) pin_array[pin].vector_len = 0;
  {
    VGA vga(renderer, 1, 0, VGA_TYPE);
    for (int n = 0; n < 2 * 800 * 525 * ratio + 10; n ++) vga.update_state();
    check_no_signal(vga);
    std::puts("PASS: unbound VGA remains no signal");
  }
  close_font();
  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  SDL_Quit();
}
