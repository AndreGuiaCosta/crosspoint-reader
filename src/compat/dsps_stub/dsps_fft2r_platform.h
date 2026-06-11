#pragma once
// Stub for PNGdec's s3_simd_rgb565.S under the pioarduino hybrid
// (custom_sdkconfig) build, whose component set doesn't ship esp-dsp
// headers to lib builds. The real header only matters on ESP32-S3; on the
// C3 the guard below evaluates false and the SIMD code assembles to nothing.
#define dsps_fft2r_sc16_aes3_enabled 0
