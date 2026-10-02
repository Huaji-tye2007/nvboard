# VGA receiver regression

From a lab project that includes NVBoard's make fragment:

```sh
make nvboard-vga-test
```

The test uses SDL's dummy video driver and checks the actual VGA receiver and
rendered framebuffer. It writes valid-video and no-signal screenshots under
`/tmp/nvboard-vga-*.bmp`.

The receiver currently supports the existing 640x480, negative-sync mode:
800 pixels per line, 525 lines per frame, 96-pixel HSYNC pulse, 2-line VSYNC
pulse, and active coordinates [144,784) by [35,515), measured from the falling
sync edges. Other timings are rejected instead of silently wrapping pixels.

Call `nvboard_update()` once per simulated source-clock cycle, including
blanking and DUT reset. Pass the number of source-clock cycles per pixel to
`nvboard_init()` (4 for 100 MHz source / 25 MHz pixels). Sampling phase is
established by HSYNC, not by the number of previously valid pixels.

A complete valid frame is published at the next VSYNC falling edge. Partial
frames are never displayed. If HSYNC is absent for two line periods or VSYNC
for two frame periods, the old image is cleared and `NO SIGNAL` is shown.
These intervals use simulated cycles, so pausing the simulation itself does
not spuriously disconnect the screen. Recovery waits for a full valid frame.

`VGA_BLANK_N`, when bound, masks pixels without changing their coordinates.
When unbound, the active region comes from the configured sync timing. RGB
may use eight-bit channels, individual pins, or mixed/packed bindings; unbound
color bits read as zero. Unbound sync inputs remain in the no-signal state.

Coverage includes ratios 1/2/4/8, black video, blanking holes, interrupted
scans, missing sync inputs, invalid sync pulse widths, reacquisition, RGB
binding layouts, and unbound ports.

For lab07, also run the actual Verilated RTL and compare every displayed pixel
against the ROM, then exercise SW0 in blank/active video at four clock phases:

```sh
make nvboard-vga-rtl-test
```

This fixture uses the project's generated Verilator 5 model and reference ROM.
It does not modify RTL or program the physical board.
