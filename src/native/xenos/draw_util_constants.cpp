/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

// tb2: the two constants spirv_translator_rb.cpp needs from the SDK's
// src/graphics/util/draw.cpp (the rest of that file isn't used here).

#include <rex/graphics/util/draw.h>

namespace rex::graphics::draw_util {

const int8_t kD3D10StandardSamplePositions2x[2][2] = {{4, 4}, {-4, -4}};
const int8_t kD3D10StandardSamplePositions4x[4][2] = {{-2, -6}, {6, -2}, {-6, 2}, {2, 6}};

}  // namespace rex::graphics::draw_util
