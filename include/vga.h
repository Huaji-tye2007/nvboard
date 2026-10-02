#ifndef __VGA_H__
#define __VGA_H__

#include <component.h>
#include <stdint.h>

#define VGA_DEFAULT_WIDTH  640
#define VGA_DEFAULT_HEIGHT 480

enum { //VGA_MOD_ID
  VGA_MODE_640_480, NR_VGA_MODE
};

struct VGA_MODE{
  int h_frontporch, h_active, h_backporch, h_total;
  int v_frontporch, v_active, v_backporch, v_total;
};

class VGA : public Component{
private:
  int vga_screen_width, vga_screen_height;
  uint32_t *pixels;
  uint32_t *capture_pixels;
  SDL_Texture *no_signal_texture;
  int line_y, captured_pixels;
  uint64_t hsync_age, vsync_age;
  bool prev_hsync, prev_vsync;
  bool have_hsync, frame_synced, frame_valid, first_line;
  bool signal_present, has_blank;
  uint8_t *p_r, *p_g, *p_b;
  bool is_r_len8, is_g_len8, is_b_len8;
  bool is_all_len8;

  uint32_t get_pixel_color_slowpath();
  void finish_one_frame();
  void lose_signal();

public:
  VGA(SDL_Renderer *rend, int cnt, int init_val, int ct);
  virtual ~VGA();

  virtual void update_gui();
  virtual void update_state();
};

#endif
