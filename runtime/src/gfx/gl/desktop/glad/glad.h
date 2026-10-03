// Desktop builds of the OpenGL renderer (headless debugging, gfx/gl/backend.cpp): libepoxy stands in
// for the glad loader the Switch build links.
#pragma once
#include <epoxy/gl.h>
typedef void* (*GLADloadproc)(const char* name);
static inline int gladLoadGLLoader(GLADloadproc) { return 1; }
