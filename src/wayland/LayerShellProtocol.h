// The generated Qt bindings for zwlr_layer_shell_v1, included from one place with the compiler asked to
// keep quiet about them.
//
// The suppression is here rather than in a flag on the whole target for two reasons. The warnings are
// real and they are not ours: wayland-scanner writes `static inline` accessors that cast a const
// listener pointer to `void **`, and -Werror turns that into a build failure of our code for somebody
// else's. And they are raised while compiling *our* translation units, because this header is included
// from ours, so no per-file option on the generated sources can silence them.
//
// It covers the include only. Nothing in this project is compiled with these warnings off.
#pragma once

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcast-qual"
#pragma GCC diagnostic ignored "-Wold-style-cast"
#pragma GCC diagnostic ignored "-Wmissing-declarations"
#pragma GCC diagnostic ignored "-Wpedantic"
#endif

#include <qwayland-wlr-layer-shell-unstable-v1.h>

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
