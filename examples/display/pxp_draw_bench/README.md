# PXP Draw Benchmark (`pxp_draw_bench`)

Hardware-accelerated 2D draw benchmark comparing NXP PXP (Pixel Pipeline) engine performance against software rendering (LVGL `lv_draw_sw_blend`) across various geometries (fills, blits, and composites).

## Dual-Depth Requirement

The QEMU gate runner (`run_qemu_pxp_draw.sh`) tests **both 16-bpp (RGB565) and 32-bpp (XRGB8888)** configurations sequentially:

1. **16-bpp (RGB565)**:
   ```sh
   cmake -B build -DCMAKE_TOOLCHAIN_FILE=../../../toolchain/rt1170-evkb.toolchain.cmake
   cmake --build build
   ```

2. **32-bpp (XRGB8888)**:
   ```sh
   cmake -B build-32 -DDRAW_BENCH_32=ON -DCMAKE_TOOLCHAIN_FILE=../../../toolchain/rt1170-evkb.toolchain.cmake
   cmake --build build-32
   ```

Both `build/pxp_draw_bench.elf` and `build-32/pxp_draw_bench.elf` must exist before executing `./run_qemu_pxp_draw.sh`.

> [!TIP]
> Using `./tools/run-all-qemu-gates.sh -b pxp_draw_bench` or the root CMake target `cmake --build build --target qemu_run_all` will automatically configure and build both depths on demand.
