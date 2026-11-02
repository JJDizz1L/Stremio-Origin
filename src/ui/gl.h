/* GL headers per platform: GLES2 on the TVs, the legacy OpenGL 2.1 profile on the Mac preview
   (it still takes GLSL 1.20 with the GLES2 subset the shaders use). */
#ifndef SB_GL_H
#define SB_GL_H
#if defined(SB_MAC) || defined(SB_LINUX)
#  if defined(__APPLE__)
#    define GL_SILENCE_DEPRECATION 1
#    include <OpenGL/gl.h>
#  else
#    define GL_GLEXT_PROTOTYPES 1
#    include <GL/gl.h>
#    include <GL/glext.h>
#  endif
#  define SB_GLSL_PRECISION ""
#else
#  include <GLES2/gl2.h>
#  define SB_GLSL_PRECISION "precision mediump float;\n"
#endif
#endif
