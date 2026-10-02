#include <nvboard.h>
#include <vga.h>
#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>

static VGA *vga = NULL;
static int vga_clk_cycles = 1;

VGA_MODE vga_mod_accepted[NR_VGA_MODE] = {
  [VGA_MODE_640_480] = {96, 144, 784, 800, 2, 35, 515, 525},
};

static bool read_sync(int pin) {
  return pin_array[pin].vector_len == 1 ? pin_peek(pin) : true;
}

// The fast path requires an entire channel bound in the usual bit order.
static bool byte_channel(int first_pin) {
  for (int bit = 0; bit < 8; bit ++) {
    const PinNode &pin = pin_array[first_pin + bit];
    if (pin.vector_len != 8 || pin.bit_offset != bit ||
        pin.ptr != pin_array[first_pin].ptr) return false;
  }
  return true;
}

static uint8_t read_channel(int first_pin) {
  uint8_t color = 0;
  for (int bit = 0; bit < 8; bit ++) {
    const PinNode &pin = pin_array[first_pin + bit];
    if (pin.vector_len == 0) continue;
    // Read only the containing byte, not a uint64_t from a smaller signal.
    const uint8_t *bytes = static_cast<const uint8_t *>(pin.ptr);
    color |= ((bytes[pin.bit_offset / 8] >> (pin.bit_offset % 8)) & 1) << bit;
  }
  return color;
}

VGA::VGA(SDL_Renderer *rend, int cnt, int init_val, int ct):
    Component(rend, cnt, init_val, ct),
    vga_screen_width(VGA_DEFAULT_WIDTH), vga_screen_height(VGA_DEFAULT_HEIGHT),
    pixels(NULL), capture_pixels(NULL), no_signal_texture(NULL),
    line_y(0), captured_pixels(0), hsync_age(0), vsync_age(0),
    prev_hsync(read_sync(VGA_HSYNC)), prev_vsync(read_sync(VGA_VSYNC)),
    have_hsync(false), frame_synced(false), frame_valid(false), first_line(true),
    signal_present(false), has_blank(pin_array[VGA_BLANK_N].vector_len == 1),
    p_r(NULL), p_g(NULL), p_b(NULL),
    is_r_len8(byte_channel(VGA_R0)), is_g_len8(byte_channel(VGA_G0)),
    is_b_len8(byte_channel(VGA_B0)),
    is_all_len8(is_r_len8 && is_g_len8 && is_b_len8) {
  SDL_Texture *texture = SDL_CreateTexture(rend, SDL_PIXELFORMAT_ARGB8888,
    SDL_TEXTUREACCESS_STREAMING, vga_screen_width, vga_screen_height);
  if (!texture) throw std::runtime_error(SDL_GetError());
  set_texture(texture, 0);
  no_signal_texture = str2texture(rend, "NO SIGNAL", 0xaaaaaa);
  pixels = new uint32_t[vga_screen_width * vga_screen_height];
  capture_pixels = new uint32_t[vga_screen_width * vga_screen_height];
  std::fill_n(pixels, vga_screen_width * vga_screen_height, 0xff000000);
  std::fill_n(capture_pixels, vga_screen_width * vga_screen_height, 0xff000000);
  SDL_Rect *rect = new SDL_Rect{0, WINDOW_HEIGHT / 2, VGA_DEFAULT_WIDTH, VGA_DEFAULT_HEIGHT};
  set_rect(rect, 0);
  if (is_r_len8) p_r = static_cast<uint8_t *>(pin_array[VGA_R0].ptr);
  if (is_g_len8) p_g = static_cast<uint8_t *>(pin_array[VGA_G0].ptr);
  if (is_b_len8) p_b = static_cast<uint8_t *>(pin_array[VGA_B0].ptr);
  update_gui();
}

VGA::~VGA() {
  SDL_DestroyTexture(no_signal_texture);
  SDL_DestroyTexture(get_texture(0));
  delete get_rect(0);
  delete []capture_pixels;
  delete []pixels;
}

void VGA::update_gui() {
  SDL_Renderer *renderer = get_renderer();
  SDL_Texture *texture = get_texture(0);
  SDL_UpdateTexture(texture, NULL, pixels, vga_screen_width * sizeof(uint32_t));
  SDL_RenderCopy(renderer, texture, NULL, get_rect(0));
  if (!signal_present) {
    int width, height;
    SDL_QueryTexture(no_signal_texture, NULL, NULL, &width, &height);
    SDL_Rect label{get_rect(0)->x + (vga_screen_width - width) / 2,
                   get_rect(0)->y + (vga_screen_height - height) / 2, width, height};
    SDL_RenderCopy(renderer, no_signal_texture, NULL, &label);
  }
  set_redraw();
}

uint32_t VGA::get_pixel_color_slowpath() {
  uint32_t r = is_r_len8 ? *p_r : read_channel(VGA_R0);
  uint32_t g = is_g_len8 ? *p_g : read_channel(VGA_G0);
  uint32_t b = is_b_len8 ? *p_b : read_channel(VGA_B0);
  return (r << 16) | (g << 8) | b;
}

void VGA::lose_signal() {
  frame_synced = false;
  frame_valid = false;
  have_hsync = false;
  if (signal_present) {
    signal_present = false;
    std::fill_n(pixels, vga_screen_width * vga_screen_height, 0xff000000);
    update_gui();
  }
}

void VGA::finish_one_frame() {
  const VGA_MODE &mode = vga_mod_accepted[VGA_MODE_640_480];
  const uint64_t expected = uint64_t(mode.h_total) * mode.v_total * vga_clk_cycles;
  if (!frame_valid || vsync_age != expected || line_y != mode.v_total - 1 ||
      captured_pixels != vga_screen_width * vga_screen_height) {
    lose_signal();
    return;
  }
  const size_t bytes = vga_screen_width * vga_screen_height * sizeof(uint32_t);
  bool changed = !signal_present || std::memcmp(pixels, capture_pixels, bytes) != 0;
  std::swap(pixels, capture_pixels);
  signal_present = true;
  if (changed) update_gui();
}

void VGA::update_state() {
  const VGA_MODE &mode = vga_mod_accepted[VGA_MODE_640_480];
  const uint64_t line_cycles = uint64_t(mode.h_total) * vga_clk_cycles;
  const uint64_t frame_cycles = line_cycles * mode.v_total;
  // Saturate counters when no source is attached or the DUT clock stops.
  if (hsync_age <= 2 * line_cycles) hsync_age ++;
  if (vsync_age <= 2 * frame_cycles) vsync_age ++;
  bool hsync = read_sync(VGA_HSYNC);
  bool vsync = read_sync(VGA_VSYNC);
  bool hfall = prev_hsync && !hsync;
  bool vfall = prev_vsync && !vsync;
  if (frame_synced && !prev_hsync && hsync &&
      hsync_age != uint64_t(mode.h_frontporch) * vga_clk_cycles) frame_valid = false;
  if (frame_synced && !prev_vsync && vsync &&
      vsync_age != uint64_t(mode.v_frontporch) * line_cycles) frame_valid = false;

  if (vfall) {
    if (frame_synced) finish_one_frame();
    frame_synced = true;
    frame_valid = hfall; // Only capture a frame whose first line is aligned.
    first_line = true;
    line_y = 0;
    captured_pixels = 0;
    vsync_age = 0;
  }
  if (hfall) {
    if (have_hsync && hsync_age != line_cycles) frame_valid = false;
    hsync_age = 0;
    have_hsync = true;
    if (frame_synced) {
      if (!first_line && line_y < mode.v_total) line_y ++;
      if (line_y >= mode.v_total) frame_valid = false;
      first_line = false;
    }
  }
  prev_hsync = hsync;
  prev_vsync = vsync;

  if (hsync_age > 2 * line_cycles || vsync_age > 2 * frame_cycles) {
    lose_signal();
    return;
  }
  if (!frame_synced || !frame_valid || !have_hsync ||
      hsync_age % vga_clk_cycles != 0) return;
  const int x = hsync_age / vga_clk_cycles - mode.h_active;
  const int y = line_y - mode.v_active;
  if (x < 0 || x >= vga_screen_width || y < 0 || y >= vga_screen_height) return;
  // Blanked pixels stay at their original coordinates instead of shifting later pixels.
  uint32_t color = 0;
  if (!has_blank || pin_peek(VGA_BLANK_N)) {
    color = is_all_len8 ? ((*p_r) << 16) | ((*p_g) << 8) | *p_b
                       : get_pixel_color_slowpath();
  }
  capture_pixels[y * vga_screen_width + x] = 0xff000000 | color;
  captured_pixels ++;
}

void vga_set_clk_cycle(int cycle) {
  // Reject overflow as well as division by zero; this is a sampling ratio.
  if (cycle <= 0 || cycle > std::numeric_limits<int>::max() / 800)
    throw std::invalid_argument("VGA clock cycle ratio is out of range");
  vga_clk_cycles = cycle;
}

static void init_render_local(SDL_Renderer *renderer) {
  SDL_SetRenderDrawColor(renderer, 0xff, 0xff, 0xff, 0);
  SDL_Point p[3];
  p[0] = Point(0, WINDOW_HEIGHT / 2) + Point(30, 0) - Point(0, CH_HEIGHT);
  p[1] = p[0] - Point(16, 0);
  p[2] = Point(p[1].x, WINDOW_HEIGHT / 2);
  draw_thicker_line(renderer, p, 3);
  draw_str(renderer, "VGA", p[0].x + 4, p[0].y - CH_HEIGHT / 2, 0xffffff);
}

void init_vga(SDL_Renderer *renderer) {
  init_render_local(renderer);
  vga = new VGA(renderer, 1, 0, VGA_TYPE);
}

void vga_update() {
  vga->update_state();
}

void quit_vga() {
  delete vga;
  vga = NULL;
}
