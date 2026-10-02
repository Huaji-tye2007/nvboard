#include <nvboard.h>
#define private public
#include <vga.h>
#undef private
#include <cstdio>
#include <cstdlib>

void vga_set_clk_cycle(int cycle);

static uint8_t red, green, blue;

static void drive(VGA &vga, int x, int y) {
  pin_array[VGA_HSYNC].data = x > 96;
  pin_array[VGA_VSYNC].data = y > 2;
  pin_array[VGA_BLANK_N].data = x > 144 && x <= 784 && y > 35 && y <= 515;
  red = (x - 145) & 255;
  green = (y - 36) & 255;
  blue = 0x5a;
  for (int cycle = 0; cycle < 4; cycle ++) vga.update_state();
}

static void frame(VGA &vga) {
  for (int y = 1; y <= 525; y ++)
    for (int x = 1; x <= 800; x ++) drive(vga, x, y);
  drive(vga, 1, 1); // publish at the next VSYNC falling edge
}

static void check_image(VGA &vga) {
  for (int y = 0; y < 480; y ++)
    for (int x = 0; x < 640; x ++) {
      uint32_t expected = ((x & 255) << 16) | ((y & 255) << 8) | 0x5a;
      if (vga.pixels[y * 640 + x] != expected) {
        std::fprintf(stderr, "ERROR: pixel (%d,%d): %06x != %06x\n",
                     x, y, vga.pixels[y * 640 + x], expected);
        std::exit(1);
      }
    }
}

int main() {
  if (SDL_Init(SDL_INIT_VIDEO) != 0) return 1;
  SDL_Window *window = SDL_CreateWindow("test", 0, 0, 1280, 960, SDL_WINDOW_HIDDEN);
  SDL_Renderer *renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
  if (!renderer) return 1;
  for (int pin = 0; pin < NR_PINS; pin ++) {
    pin_array[pin].ptr = &pin_array[pin].data;
    pin_array[pin].vector_len = 1;
  }
  pin_array[VGA_R0].ptr = &red;
  pin_array[VGA_G0].ptr = &green;
  pin_array[VGA_B0].ptr = &blue;
  pin_array[VGA_R0].vector_len = 8;
  pin_array[VGA_G0].vector_len = 8;
  pin_array[VGA_B0].vector_len = 8;
  vga_set_clk_cycle(4);
  {
    VGA vga(renderer, 1, 0, VGA_TYPE);
    frame(vga);
    check_image(vga);
    for (int row : {36, 100, 515}) {
      for (int y = 1; y <= row; y ++)
        for (int x = 1; x <= (y == row ? 300 : 800); x ++) drive(vga, x, y);
      uint32_t first_pixel = vga.pixels[0];
      red = 0xee; green = 0xcc; blue = 0xaa;
      for (int cycle = 0; cycle < 640 * 480 * 4; cycle ++) vga.update_state();
      if (vga.pixel_x != 640 || vga.pixels[0] != first_pixel) {
        std::fprintf(stderr, "ERROR: stopped scan escaped row %d (x=%d)\n", row, vga.pixel_x);
        return 1;
      }
      frame(vga);
      check_image(vga);
    }
    pin_array[VGA_BLANK_N].data = 0;
    for (int cycle = 0; cycle < 10000; cycle ++) vga.update_state();
    check_image(vga);
    std::puts("PASS: complete frames, stopped active scan, blanking and frame resynchronization");
  }
  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  SDL_Quit();
}
