#pragma once

// Disable the SDK's default behavior of panicking on malloc failure so
// we can detect allocation issues and gracefully fall back to a built-in ROM.
#define PICO_USE_MALLOC_PANIC 0
