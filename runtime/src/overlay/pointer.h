#pragma once
#include "imgui.h"
#include <cfloat>
#include <cmath>

namespace overlay {
// The pointer lives on the same logical canvas as the UI and GamePad layout.
// The compositor packs both the canvas and its software pointer for each eye.
// Folding host coordinates at the eye divider would make drags wrap halfway.
inline ImVec2 pointer_position(float nx,float ny,const ImVec2& size) {
    if(!std::isfinite(nx)||!std::isfinite(ny)||nx<=-FLT_MAX/2||ny<=-FLT_MAX/2)
        return {-FLT_MAX,-FLT_MAX};
    return {nx*size.x,ny*size.y};
}
}
