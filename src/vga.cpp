#include <nvboard.h>
#include <vga.h>
#include <macro.h>

static VGA* vga = NULL;

VGA_MODE vga_mod_accepted[NR_VGA_MODE] = {
  [VGA_MODE_640_480] = {
    .h_frontporch = 96,
    .h_active = 144,
    .h_backporch = 784,
    .h_total = 800,
    .v_frontporch = 2,
    .v_active = 35,
    .v_backporch = 515,
    .v_total = 525,
  },
};

static int vga_clk_cycle_minus_1 = 0;
uint8_t *vga_blank_n_ptr = NULL;

VGA::VGA(SDL_Renderer *rend, int cnt, int init_val, int ct):
    Component(rend, cnt, init_val, ct),
    vga_screen_width(VGA_DEFAULT_WIDTH), vga_screen_height(VGA_DEFAULT_HEIGHT),
    vga_clk_cnt(1), pixel_x(0), pixel_y(-1),
    prev_hsync(true), prev_vsync(true), frame_synced(false),
    line_started(false), is_pixels_same(true) {
  SDL_Texture *vga_texture = SDL_CreateTexture(rend, SDL_PIXELFORMAT_ARGB8888,
    SDL_TEXTUREACCESS_STREAMING, vga_screen_width, vga_screen_height);
  set_texture(vga_texture, 0);
  pixels = new uint32_t[vga_screen_width * vga_screen_height];
  memset(pixels, 0, vga_screen_width * vga_screen_height * sizeof(uint32_t));

  SDL_Rect *rect_ptr = new SDL_Rect;
  *rect_ptr = (SDL_Rect){0, WINDOW_HEIGHT / 2, VGA_DEFAULT_WIDTH, VGA_DEFAULT_HEIGHT};
  set_rect(rect_ptr, 0);
  SDL_UpdateTexture(vga_texture, NULL, pixels, vga_screen_width * sizeof(uint32_t));
  SDL_RenderCopy(rend, vga_texture, NULL, rect_ptr);

  is_r_len8 = pin_array[VGA_R0].vector_len == 8;
  is_g_len8 = pin_array[VGA_G0].vector_len == 8;
  is_b_len8 = pin_array[VGA_B0].vector_len == 8;
  is_all_len8 = is_r_len8 && is_g_len8 && is_b_len8;
  if (is_r_len8) p_r = (uint8_t *)pin_array[VGA_R0].ptr;
  if (is_g_len8) p_g = (uint8_t *)pin_array[VGA_G0].ptr;
  if (is_b_len8) p_b = (uint8_t *)pin_array[VGA_B0].ptr;
  int vga_blank_n_len = pin_array[VGA_BLANK_N].vector_len;
  assert(vga_blank_n_len == 1 || vga_blank_n_len == 0);
  vga_blank_n_ptr = (uint8_t *)pin_array[VGA_BLANK_N].ptr;
}

VGA::~VGA() {
  SDL_DestroyTexture(get_texture(0));
  delete []pixels;
}

void VGA::update_gui() {
#ifdef DEBUG
  static int frames = 0;
  frames ++;
  printf("%d frames\n", frames);
#endif
  SDL_Texture *vga_texture = get_texture(0);
  SDL_UpdateTexture(vga_texture, NULL, pixels, vga_screen_width * sizeof(uint32_t));
  SDL_RenderCopy(get_renderer(), vga_texture, NULL, get_rect(0));
  set_redraw();
}

uint32_t VGA::get_pixel_color_slowpath() {
#define concat3(a, b, c) concat(concat(a, b), c)
#define MAP2(c, f, x)  c(f, x)
#define GET_COLOR_BIT(color, n) (pin_peek(concat3(VGA_, color, n)) << n)
#define BITS(f, color) f(color, 0) f(color, 1) f(color, 2) f(color, 3) \
                       f(color, 4) f(color, 5) f(color, 6) f(color, 7)
#define GET_COLOR_BIT_REDUCE(color, n) GET_COLOR_BIT(color, n) |
#define GET_COLOR(color) MAP2(BITS, GET_COLOR_BIT_REDUCE, color) 0
  int r = is_r_len8 ? *p_r : GET_COLOR(R);
  int g = is_g_len8 ? *p_g : GET_COLOR(G);
  int b = is_b_len8 ? *p_b : GET_COLOR(B);
  uint32_t color = (r << 16) | (g << 8) | b;
  return color;
}

__attribute__((noinline)) void VGA::finish_one_frame() {
  if (!is_pixels_same) {
    update_gui();
    is_pixels_same = true;
  }
}

void VGA::update_state() {
  // Observe sync even during blanking; the pixel clock divider keeps running.
  bool hsync = pin_peek(VGA_HSYNC);
  bool vsync = pin_peek(VGA_VSYNC);
  if (prev_vsync && !vsync) {
    if (frame_synced) finish_one_frame();
    frame_synced = true;
    pixel_x = 0;
    pixel_y = -1;
    line_started = false;
  }
  if (prev_hsync && !hsync) {
    pixel_x = 0;
    line_started = false;
    vga_clk_cnt = 0;
  }
  prev_hsync = hsync;
  prev_vsync = vsync;

  if (unlikely(vga_clk_cycle_minus_1 > 0)) {
    if (vga_clk_cnt > 0) { vga_clk_cnt --; return; }
    vga_clk_cnt = vga_clk_cycle_minus_1;
  }

  if (!frame_synced || !hsync || !vsync || !*vga_blank_n_ptr) return;
  if (!line_started) {
    line_started = true;
    pixel_y ++;
  }
  // A stopped DUT clock may leave blank_n high. Never wrap into another line.
  if (pixel_x >= vga_screen_width || pixel_y >= vga_screen_height) return;

  uint32_t color = 0;
  if (likely(is_all_len8)) color = ((*p_r) << 16) | ((*p_g) << 8) | (*p_b);
  else                     color = get_pixel_color_slowpath();
  uint32_t *pixel = pixels + pixel_y * vga_screen_width + pixel_x;
  bool is_same = (*pixel == color);
  is_pixels_same &= is_same;
  *pixel = color;
  pixel_x ++;
}

void vga_set_clk_cycle(int cycle) {
  vga_clk_cycle_minus_1 = cycle - 1;
}

static void init_render_local(SDL_Renderer *renderer) {
  // draw line
  SDL_SetRenderDrawColor(renderer, 0xff, 0xff, 0xff, 0);
  SDL_Point p[3];
  p[0] = Point(0, WINDOW_HEIGHT / 2) + Point(30, 0) - Point(0, CH_HEIGHT);
  p[1] = p[0] - Point(16, 0);
  p[2] = Point(p[1].x,  WINDOW_HEIGHT / 2);
  draw_thicker_line(renderer, p, 3);

  // draw label
  draw_str(renderer, "VGA", p[0].x + 4, p[0].y - CH_HEIGHT / 2, 0xffffff);
}

void init_vga(SDL_Renderer *renderer) {
  init_render_local(renderer);
  vga = new VGA(renderer, 1, 0, VGA_TYPE);
  for (int p = VGA_VSYNC; p <= VGA_B7; p ++) {
    vga->add_pin(p);
  }
}

void vga_update() {
  vga->update_state();
}
