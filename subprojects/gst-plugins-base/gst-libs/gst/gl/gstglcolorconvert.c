/*
 * GStreamer
 * Copyright (C) 2012-2014 Matthew Waters <ystree00@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <string.h>
#include <stdio.h>

#include "gstglcolorconvert.h"

#include "gl.h"
#include "gstglfuncs.h"
#include "gstglsl_private.h"

/**
 * SECTION:gstglcolorconvert
 * @title: GstGLColorConvert
 * @short_description: convert between video color spaces and formats
 * @see_also: #GstGLUpload, #GstGLMemory, #GstGLBaseMemory
 *
 * #GstGLColorConvert is an object that converts between color spaces and/or
 * formats using OpenGL Shaders.
 *
 * A #GstGLColorConvert can be created with gst_gl_color_convert_new(), the
 * configuration negotiated with gst_gl_color_convert_transform_caps() and the
 * conversion performed with gst_gl_color_convert_perform().
 *
 * The glcolorconvertelement provides a GStreamer element that uses
 * #GstGLColorConvert to convert between video formats and color spaces.
 */

/* TODO: by default videoconvert does not convert primaries, but this may
 * be an option in the future */

#define USING_OPENGL(context) (gst_gl_context_check_gl_version (context, GST_GL_API_OPENGL, 1, 0))
#define USING_OPENGL3(context) (gst_gl_context_check_gl_version (context, GST_GL_API_OPENGL3, 3, 1))
#define USING_OPENGL30(context) \
  (gst_gl_context_check_gl_version (context, GST_GL_API_OPENGL, 3, 0) || USING_OPENGL3(context))
#define USING_GLES(context) (gst_gl_context_check_gl_version (context, GST_GL_API_GLES, 1, 0))
#define USING_GLES2(context) (gst_gl_context_check_gl_version (context, GST_GL_API_GLES2, 2, 0))
#define USING_GLES3(context) (gst_gl_context_check_gl_version (context, GST_GL_API_GLES2, 3, 0))

static void _do_convert (GstGLContext * context, GstGLColorConvert * convert);
static gboolean _init_convert (GstGLColorConvert * convert);
static gboolean _init_convert_fbo (GstGLColorConvert * convert);
static GstBuffer *_gst_gl_color_convert_perform_unlocked (GstGLColorConvert *
    convert, GstBuffer * inbuf);

static gboolean _do_convert_draw (GstGLContext * context,
    GstGLColorConvert * convert);

/* *INDENT-OFF* */

#define YUV_TO_RGB_COEFFICIENTS \
      "uniform mat4 to_RGB_matrix;\n" \

#define RGB_TO_YUV_COEFFICIENTS \
      "uniform mat4 to_YUV_matrix;\n" \

/* GRAY16 to RGB conversion
 *  data transferred as GL_LUMINANCE_ALPHA then convert back to GRAY16
 *  high byte weight as : 255*256/65535
 *  ([0~1] denormalize to [0~255],shift to high byte,normalize to [0~1])
 *  low byte weight as : 255/65535 (similar)
 * */
#define COMPOSE_WEIGHT \
    "const vec2 compose_weight = vec2(0.996109, 0.003891);\n"

#define DEFAULT_UNIFORMS         \
    "uniform vec2 tex_scale0;\n" \
    "uniform vec2 tex_scale1;\n" \
    "uniform vec2 tex_scale2;\n" \
    "uniform vec2 tex_scale3;\n" \
    "uniform float width;\n"     \
    "uniform float height;\n"    \
    "uniform float poffset_x;\n" \
    "uniform float poffset_y;\n" \
    "uniform int input_swizzle[4];\n" \
    "uniform int output_swizzle[4];\n"

#define MAX_FUNCTIONS 5

#define glsl_OES_extension_string "#extension GL_OES_EGL_image_external : require \n"

struct shader_templ
{
  const gchar *extensions;
  const gchar *uniforms;
  const gchar *functions[MAX_FUNCTIONS];

  GstGLTextureTarget target;
};

static const char glsl_func_color_matrix[] = \
    "vec4 color_matrix_apply (vec4 texel, mat4 colormatrix) {\n" \
    // ignore alpha applying the RGB/YUV color matrix and copy alpha over to
    // the output
    "  float a = texel.a;\n" \
    "  texel.a = 1.0;\n" \
    "  vec4 ret = colormatrix * texel;\n" \
    "  ret.a = a;\n" \
    "  return ret;\n"
    "}\n";

static const char glsl_func_swizzle[] = "vec4 swizzle(vec4 texel, int components[4]) {\n" \
  "  return vec4(texel[components[0]], texel[components[1]], texel[components[2]], texel[components[3]]);\n" \
  "}\n" \
  "vec3 swizzle(vec3 texel, int components[3]) {\n" \
  "  return vec3(texel[components[0]], texel[components[1]], texel[components[2]]);\n" \
  "}\n" \
  "vec2 swizzle(vec2 texel, int components[2]) {\n" \
  "  return vec2(texel[components[0]], texel[components[1]]);\n" \
  "}\n" \
  "vec2 swizzle2(vec3 texel, int components[3]) {\n" \
  "  return vec2(texel[components[0]], texel[components[1]]);\n" \
  "}\n" \
  "vec2 swizzle2(vec4 texel, int components[4]) {\n" \
  "  return vec2(texel[components[0]], texel[components[1]]);\n" \
  "}\n" \
  "vec3 swizzle3(vec4 texel, int components[4]) {\n" \
  "  return vec3(texel[components[0]], texel[components[1]], texel[components[2]]);\n" \
  "}\n";

/* Channel reordering for XYZ <-> ZYX conversion */
static const gchar templ_REORDER_BODY[] =
    "vec4 t = swizzle(texture2D(tex, texcoord * tex_scale0), input_swizzle);\n"
    "gl_FragColor = vec4(swizzle(t, output_swizzle));\n";

static const struct shader_templ templ_REORDER =
  { NULL,
    DEFAULT_UNIFORMS "uniform sampler2D tex;\n",
    { glsl_func_swizzle, },
    GST_GL_TEXTURE_TARGET_2D
  };

/* Channel reordering for XYZ <-> ZYX conversion */
static const gchar templ_REORDER_OVERWRITE_ALPHA_BODY[] =
    "vec4 t = swizzle(texture2D(tex, texcoord * tex_scale0), input_swizzle);\n"
    "t.a = 1.0;\n" /* clobber alpha channel? */
    "gl_FragColor = vec4(swizzle(t, output_swizzle));\n";

static const struct shader_templ templ_REORDER_OVERWRITE_ALPHA =
  { NULL,
    DEFAULT_UNIFORMS "uniform sampler2D tex;\n",
    { glsl_func_swizzle, },
    GST_GL_TEXTURE_TARGET_2D
  };

/* GRAY16 to RGB conversion
 *  data transferred as GL_LUMINANCE_ALPHA then convert back to GRAY16
 *  high byte weight as : 255*256/65535
 *  ([0~1] denormalize to [0~255],shift to high byte,normalize to [0~1])
 *  low byte weight as : 255/65535 (similar)
 * */
static const gchar templ_COMPOSE_BODY[] =
    "vec4 rgba;\n"
    "vec4 t = texture2D(tex, texcoord * tex_scale0);\n"
    "rgba.rgb = vec3 (dot(swizzle2(t, input_swizzle), compose_weight));\n"
    "rgba.a = 1.0;\n"
    "gl_FragColor = swizzle(rgba, output_swizzle);\n";

static const struct shader_templ templ_COMPOSE =
  { NULL,
    DEFAULT_UNIFORMS COMPOSE_WEIGHT "uniform sampler2D tex;\n",
    { glsl_func_swizzle, },
    GST_GL_TEXTURE_TARGET_2D
  };

/* Shaders for AYUV and varieties */
static const gchar templ_AYUV_to_RGB_BODY[] =
    "vec4 texel = swizzle(texture2D(tex, texcoord * tex_scale0), input_swizzle);\n"
    "vec4 rgba = color_matrix_apply(texel, to_RGB_matrix);\n"
    "gl_FragColor = swizzle(rgba, output_swizzle);\n";

static const struct shader_templ templ_AYUV_to_RGB =
  { NULL,
    DEFAULT_UNIFORMS YUV_TO_RGB_COEFFICIENTS "uniform sampler2D tex;\n",
    { glsl_func_swizzle, glsl_func_color_matrix, NULL, },
    GST_GL_TEXTURE_TARGET_2D
  };

static const gchar templ_RGB_to_AYUV_BODY[] =
    "vec4 texel = swizzle(texture2D(tex, texcoord), input_swizzle);\n"
    "vec4 yuva = color_matrix_apply(texel, to_YUV_matrix);\n"
    "yuva.a = %s;\n"
    "gl_FragColor = swizzle(yuva, output_swizzle);\n";

static const struct shader_templ templ_RGB_to_AYUV =
  { NULL,
    DEFAULT_UNIFORMS RGB_TO_YUV_COEFFICIENTS "uniform sampler2D tex;\n",
    { glsl_func_swizzle, glsl_func_color_matrix, NULL, },
    GST_GL_TEXTURE_TARGET_2D
  };

static const char glsl_func_fetch_planar_yuv[] =
    "vec4 fetch_planar_yuv(sampler2D Ytex, sampler2D Utex, sampler2D Vtex, vec2 texcoord) {\n"
    "  vec4 yuva;\n"
    "  yuva.x = texture2D(Ytex, texcoord * tex_scale0).r * in_bitdepth_factor;\n"
    "  yuva.y = texture2D(Utex, texcoord * tex_scale1).r * in_bitdepth_factor;\n"
    "  yuva.z = texture2D(Vtex, texcoord * tex_scale2).r * in_bitdepth_factor;\n"
    "  yuva.a = 1.0;\n"
    "  return yuva;\n"
    "}\n"
    "vec4 fetch_planar_yuva(sampler2D Ytex, sampler2D Utex, sampler2D Vtex, sampler2D Atex, vec2 texcoord) {\n"
    "  vec4 yuva = fetch_planar_yuv(Ytex, Utex, Vtex, texcoord);\n"
    "  yuva.a = texture2D(Atex, texcoord * tex_scale3).r * in_bitdepth_factor;\n"
    "  return yuva;\n"
    "}\n";

/* YUV to RGB conversion */
static const gchar templ_PLANAR_YUV_to_RGB_BODY[] =
    /* FIXME: should get the sampling right... */
    "vec4 yuva = fetch_planar_yuv(Ytex, Utex, Vtex, texcoord);\n"
    "yuva = swizzle(yuva, input_swizzle);\n"
    "vec4 rgba = color_matrix_apply(yuva, to_RGB_matrix);\n"
    "gl_FragColor = swizzle(rgba, output_swizzle);\n";

static const struct shader_templ templ_PLANAR_YUV_to_RGB =
  { NULL,
    DEFAULT_UNIFORMS YUV_TO_RGB_COEFFICIENTS "uniform sampler2D Ytex, Utex, Vtex;\n" "uniform float in_bitdepth_factor;\n",
    { glsl_func_swizzle, glsl_func_color_matrix, glsl_func_fetch_planar_yuv, NULL, },
    GST_GL_TEXTURE_TARGET_2D
  };

static const gchar templ_PLANAR_YUVA_to_RGB_BODY[] =
    /* FIXME: should get the sampling right... */
    "vec4 yuva = fetch_planar_yuva(Ytex, Utex, Vtex, Atex, texcoord);\n"
    "yuva = swizzle(yuva, input_swizzle);\n"
    "vec4 rgba = color_matrix_apply(yuva, to_RGB_matrix);\n"
    "gl_FragColor = swizzle(rgba, output_swizzle);\n";

static const struct shader_templ templ_A420_to_RGB =
  { NULL,
    /* 4th uniform is the alpha buffer */
    DEFAULT_UNIFORMS YUV_TO_RGB_COEFFICIENTS "uniform sampler2D Ytex, Utex, Vtex, Atex;\n" "uniform float in_bitdepth_factor;\n",
    { glsl_func_swizzle, glsl_func_color_matrix, glsl_func_fetch_planar_yuv, NULL, },
    GST_GL_TEXTURE_TARGET_2D
  };

static const char glsl_func_chroma_sample[] =
    "vec4 chroma_sample(sampler2D tex, vec2 texcoord, vec2 unnormalization) {\n"
    "  vec4 uv_texel = vec4(0.0);\n"
    /* One u and v sample can be generated by a nxm sized block given by
     * @chroma_sampling.  The result is the average of all the values in the
     * block computed with a rolling average.
     */
     /* scale for chroma size */
    "  vec2 chroma_pos = texcoord * chroma_sampling * unnormalization;\n"
     /* offset chroma to the center of the first texel in the block */
    "  chroma_pos -= clamp(chroma_sampling * 0.5 - 0.5, vec2(0.0), chroma_sampling);\n"
    "  if (chroma_pos.x < width && chroma_pos.y < height) {\n"
    "    for (int i = 0; i < int(chroma_sampling.x); i++) {\n"
    "      vec2 delta = vec2 (float(i), 0.0);\n"
    "      for (int j = 0; j < int(chroma_sampling.y); j++) {\n"
    "        int n = (i+1)*(j+1);\n"
    "        delta.y = float(j);\n"
    "        vec4 s = swizzle(texture2D(tex, (chroma_pos + delta) / unnormalization), input_swizzle);\n"
             /* rolling average */
    "        uv_texel = (float(n-1) * uv_texel + s) / float(n);\n"
    "      }\n"
    "    }\n"
    "  }\n"
    "  return uv_texel;\n"
    "}\n";

static const char glsl_func_write_planar_yuv[] =
    "void write_planar_yuv(vec4 yuva) {\n"
    "  gl_FragData[0] = vec4(yuva.x, 0.0, 0.0, 1.0);\n"
    "  gl_FragData[1] = vec4(yuva.y, 0.0, 0.0, 1.0);\n"
    "  gl_FragData[2] = vec4(yuva.z, 0.0, 0.0, 1.0);\n"
    "}\n";
static const char glsl_func_write_planar_yuva[] =
    "void write_planar_yuva(vec4 yuva) {\n"
    "  gl_FragData[0] = vec4(yuva.x, 0.0, 0.0, 1.0);\n"
    "  gl_FragData[1] = vec4(yuva.y, 0.0, 0.0, 1.0);\n"
    "  gl_FragData[2] = vec4(yuva.z, 0.0, 0.0, 1.0);\n"
    "  gl_FragData[3] = vec4(yuva.a, 0.0, 0.0, 1.0);\n"
    "}\n";

static const gchar templ_RGB_to_PLANAR_YUV_BODY[] =
    "vec4 texel = swizzle(texture2D(tex, texcoord), input_swizzle);\n"
    "vec2 unnormalization;\n"
    "if (texcoord.x == v_texcoord.x) {\n"
    "  unnormalization = vec2(width, height);\n"
    "} else {\n"
    "  unnormalization = vec2 (1.0);\n"
    "}\n"
    "vec4 uv_texel = chroma_sample(tex, texcoord, unnormalization);\n"
    "vec4 yuva;\n"
    "yuva.x = color_matrix_apply(texel, to_YUV_matrix).x;\n"
    "yuva.yz = color_matrix_apply(uv_texel, to_YUV_matrix).yz;\n"
    "yuva.a = texel.a;\n"
    "yuva = swizzle(yuva, output_swizzle) * out_bitdepth_factor;\n"
    "write_planar_yuv(yuva);\n";

static const struct shader_templ templ_RGB_to_PLANAR_YUV =
  { NULL,
    DEFAULT_UNIFORMS RGB_TO_YUV_COEFFICIENTS "uniform sampler2D tex;\n"
    "uniform vec2 chroma_sampling;\n" "uniform float out_bitdepth_factor;\n",
    { glsl_func_swizzle, glsl_func_color_matrix, glsl_func_chroma_sample, glsl_func_write_planar_yuv, NULL, },
    GST_GL_TEXTURE_TARGET_2D
  };

static const gchar templ_RGB_to_PLANAR_YUVA_BODY[] =
    "vec4 texel = swizzle(texture2D(tex, texcoord), input_swizzle);\n"
    "vec2 unnormalization;\n"
    "if (texcoord.x == v_texcoord.x) {\n"
    "  unnormalization = vec2(width, height);\n"
    "} else {\n"
    "  unnormalization = vec2 (1.0);\n"
    "}\n"
    "vec4 uv_texel = chroma_sample(tex, texcoord, unnormalization);\n"
    "vec4 yuva;\n"
    "yuva.x = color_matrix_apply(texel, to_YUV_matrix).x;\n"
    "yuva.yz = color_matrix_apply(uv_texel, to_YUV_matrix).yz;\n"
    "yuva.a = texel.a;\n"
    "yuva = swizzle(yuva, output_swizzle) * out_bitdepth_factor;\n"
    "write_planar_yuva(yuva);\n";

static const struct shader_templ templ_RGB_to_PLANAR_YUVA =
  { NULL,
    DEFAULT_UNIFORMS RGB_TO_YUV_COEFFICIENTS "uniform sampler2D tex;\n"
    "uniform vec2 chroma_sampling;\n" "uniform float out_bitdepth_factor;\n",
    { glsl_func_swizzle, glsl_func_color_matrix, glsl_func_chroma_sample, glsl_func_write_planar_yuva, NULL, },
    GST_GL_TEXTURE_TARGET_2D
  };

static const gchar templ_PLANAR_YUV_to_PLANAR_YUV_BODY[] =
    "vec4 yuva;\n"
    "yuva.x = swizzle(texture2D(Ytex, texcoord * tex_scale0), input_swizzle).r * in_bitdepth_factor;\n"
    "yuva.y = swizzle(texture2D(Utex, texcoord * tex_scale1 * chroma_sampling), input_swizzle).r * in_bitdepth_factor;\n"
    "yuva.z = swizzle(texture2D(Vtex, texcoord * tex_scale2 * chroma_sampling), input_swizzle).r * in_bitdepth_factor;\n"
    "yuva.a = 1.0;\n"
    "yuva = swizzle(yuva, output_swizzle) * out_bitdepth_factor;\n"
    "write_planar_yuv(yuva);\n";

static const struct shader_templ templ_PLANAR_YUV_to_PLANAR_YUV =
  { NULL,
    DEFAULT_UNIFORMS RGB_TO_YUV_COEFFICIENTS "uniform sampler2D Ytex, Utex, Vtex;\n"
    "uniform vec2 chroma_sampling;\n" "uniform float in_bitdepth_factor;\n" "uniform float out_bitdepth_factor;\n",
    { glsl_func_swizzle, glsl_func_color_matrix, glsl_func_write_planar_yuv, NULL, },
    GST_GL_TEXTURE_TARGET_2D
  };

static const gchar templ_PLANAR_YUV_to_PLANAR_YUVA_BODY[] =
    "vec4 yuva;\n"
    "yuva.x = swizzle(texture2D(Ytex, texcoord * tex_scale0), input_swizzle).r * in_bitdepth_factor;\n"
    "yuva.y = swizzle(texture2D(Utex, texcoord * tex_scale1 * chroma_sampling), input_swizzle).r * in_bitdepth_factor;\n"
    "yuva.z = swizzle(texture2D(Vtex, texcoord * tex_scale2 * chroma_sampling), input_swizzle).r * in_bitdepth_factor;\n"
    "yuva.a = 1.0;\n"
    "yuva = swizzle(yuva, output_swizzle) * out_bitdepth_factor;\n"
    "write_planar_yuva(yuva);\n";

static const struct shader_templ templ_PLANAR_YUV_to_PLANAR_YUVA =
  { NULL,
    DEFAULT_UNIFORMS RGB_TO_YUV_COEFFICIENTS "uniform sampler2D Ytex, Utex, Vtex;\n"
    "uniform vec2 chroma_sampling;\n" "uniform float in_bitdepth_factor;\n" "uniform float out_bitdepth_factor;\n",
    { glsl_func_swizzle, glsl_func_color_matrix, glsl_func_write_planar_yuva, NULL, },
    GST_GL_TEXTURE_TARGET_2D
  };

static const gchar templ_PLANAR_YUVA_to_PLANAR_YUVA_BODY[] =
    "vec4 yuva;\n"
    "yuva.x = swizzle(texture2D(Ytex, texcoord * tex_scale0), input_swizzle).r * in_bitdepth_factor;\n"
    "yuva.y = swizzle(texture2D(Utex, texcoord * tex_scale1 * chroma_sampling), input_swizzle).r * in_bitdepth_factor;\n"
    "yuva.z = swizzle(texture2D(Vtex, texcoord * tex_scale2 * chroma_sampling), input_swizzle).r * in_bitdepth_factor;\n"
    "yuva.a = swizzle(texture2D(Atex, texcoord * tex_scale3), input_swizzle).r;\n"
    "yuva = swizzle(yuva, output_swizzle) * out_bitdepth_factor;\n"
    "write_planar_yuva(yuva);\n";

static const struct shader_templ templ_PLANAR_YUVA_to_PLANAR_YUVA =
  { NULL,
    DEFAULT_UNIFORMS RGB_TO_YUV_COEFFICIENTS "uniform sampler2D Ytex, Utex, Vtex, Atex;\n"
    "uniform vec2 chroma_sampling;\n" "uniform float in_bitdepth_factor;\n" "uniform float out_bitdepth_factor;\n",
    { glsl_func_swizzle, glsl_func_color_matrix, glsl_func_write_planar_yuv, glsl_func_write_planar_yuva, NULL, },
    GST_GL_TEXTURE_TARGET_2D
  };

static const gchar templ_PLANAR_YUVA_to_PLANAR_YUV_BODY[] =
    "vec4 yuva;\n"
    "yuva.x = swizzle(texture2D(Ytex, texcoord * tex_scale0), input_swizzle).r * in_bitdepth_factor;\n"
    "yuva.y = swizzle(texture2D(Utex, texcoord * tex_scale1 * chroma_sampling), input_swizzle).r * in_bitdepth_factor;\n"
    "yuva.z = swizzle(texture2D(Vtex, texcoord * tex_scale2 * chroma_sampling), input_swizzle).r * in_bitdepth_factor;\n"
    "yuva.a = swizzle(texture2D(Atex, texcoord * tex_scale3), input_swizzle).r;\n"
    "yuva = swizzle(yuva, output_swizzle) * out_bitdepth_factor;\n"
    "write_planar_yuv(yuva);\n";

static const struct shader_templ templ_PLANAR_YUVA_to_PLANAR_YUV =
  { NULL,
    DEFAULT_UNIFORMS RGB_TO_YUV_COEFFICIENTS "uniform sampler2D Ytex, Utex, Vtex, Atex;\n"
    "uniform vec2 chroma_sampling;\n" "uniform float in_bitdepth_factor;\n" "uniform float out_bitdepth_factor;\n",
    { glsl_func_swizzle, glsl_func_color_matrix, glsl_func_chroma_sample, glsl_func_write_planar_yuv, NULL, },
    GST_GL_TEXTURE_TARGET_2D
  };

/* semi-planar to RGB conversion */
static const gchar templ_SEMI_PLANAR_to_RGB_BODY[] =
    "vec4 yuva;\n"
    /* FIXME: should get the sampling right... */
    "yuva.x=texture2D(Ytex, texcoord * tex_scale0).r;\n"
    "yuva.yz=texture2D(UVtex, texcoord * tex_scale1).r%c;\n"
    "%s"
    "yuva = swizzle(yuva, input_swizzle);\n"
    "vec4 rgba = color_matrix_apply (yuva, to_RGB_matrix);\n"
    "gl_FragColor = swizzle(rgba, output_swizzle);\n";

static const struct shader_templ templ_SEMI_PLANAR_to_RGB =
  { NULL,
    DEFAULT_UNIFORMS YUV_TO_RGB_COEFFICIENTS "uniform sampler2D Ytex, UVtex;\n",
    { glsl_func_swizzle, glsl_func_color_matrix, NULL, },
    GST_GL_TEXTURE_TARGET_2D
  };

static const struct shader_templ templ_AV12_to_RGB =
  { NULL,
    DEFAULT_UNIFORMS YUV_TO_RGB_COEFFICIENTS "uniform sampler2D Ytex, UVtex, Atex;\n",
    { glsl_func_swizzle, glsl_func_color_matrix, NULL, },
    GST_GL_TEXTURE_TARGET_2D
  };

#define glsl_func_frag_to_tile \
    "ivec2 frag_to_tile(ivec2 tile_coord, ivec2 delta_coord, ivec2 dim, int width, int tiles_per_row, int need_offset) {\n" \
    "  int tile_size = (dim.x * dim.y);\n" \
    "  int tile_index = tile_coord.y * tiles_per_row + tile_coord.x;\n" \
    "  int linear_index = tile_index * tile_size + delta_coord.y * dim.x + delta_coord.x;\n" \
    "  linear_index += need_offset * tile_size / 2;\n" \
    "  return ivec2(linear_index % width, linear_index / width);\n" \
    "}\n"

/* TILED semi-planar to RGB conversion */
static const gchar templ_TILED_SEMI_PLANAR_to_RGB_BODY[] =
    "  vec4 yuva;\n"
    "  ivec2 texel;\n"
    "\n"
    "  const ivec2 luma_dim = ivec2(%i, %i);\n"
    "  const ivec2 chroma_dim = ivec2(%i, %i);\n"
    "  const int fy = chroma_dim.y * 2 / luma_dim.y;\n"
    "\n"
    "  int iwidth = int(width);\n"
    "  int tiles_per_row = iwidth / luma_dim.x;\n"
    "\n"
    "  ivec2 coord = ivec2(gl_FragCoord.xy);\n"
    "  ivec2 tile_coord = coord / luma_dim;\n"
    "  ivec2 delta_coord = coord %% luma_dim;\n" \
    "  texel = frag_to_tile(tile_coord, delta_coord, luma_dim, iwidth, tiles_per_row, 0);\n"
    "  yuva.x = texelFetch(Ytex, texel, 0).r;\n"
    "\n"
    "  ivec2 chroma_tcoord = ivec2(tile_coord.x, tile_coord.y / fy);\n"
    "  texel = frag_to_tile(chroma_tcoord, delta_coord / 2, chroma_dim, iwidth / 2, tiles_per_row, tile_coord.y %% fy);\n"
    "  yuva.yz = texelFetch(UVtex, texel, 0).%c%c;\n"
    "  yuva.a = 1.0;\n"
    "\n"
    "  vec4 rgba = color_matrix_apply(yuva, to_RGB_matrix);\n"
    "  gl_FragColor = swizzle(rgba, output_swizzle);\n";

static const struct shader_templ templ_TILED_SEMI_PLANAR_to_RGB =
  { NULL,
    DEFAULT_UNIFORMS YUV_TO_RGB_COEFFICIENTS "uniform sampler2D Ytex, UVtex;\n",
    { glsl_func_swizzle, glsl_func_color_matrix, glsl_func_frag_to_tile, NULL, },
    GST_GL_TEXTURE_TARGET_2D
  };

  /* RGB to NV12/NV21/NV16/NV61 conversion */
  /* NV12/NV16: u, v
     NV21/NV61: v, u */
static const gchar templ_RGB_to_SEMI_PLANAR_YUV_BODY[] =
    "vec4 texel, uv_texel;\n"
    "vec4 yuva;\n"
    "texel = swizzle(texture2D(tex, texcoord), input_swizzle);\n"
    "uv_texel = swizzle(texture2D(tex, texcoord * tex_scale0 * chroma_sampling), input_swizzle);\n"
    "yuva.x = color_matrix_apply(texel, to_YUV_matrix).x;\n"
    "yuva.yz = color_matrix_apply(uv_texel, to_YUV_matrix).yz;\n"
    "yuva.a = 1.0;\n"
    "yuva = swizzle(yuva, output_swizzle);\n"
    "gl_FragData[0] = vec4(yuva.x, 0.0, 0.0, 1.0);\n"
    "gl_FragData[1] = vec4(yuva.y, yuva.z, 0.0, 1.0);\n"
    "%s";

static const struct shader_templ templ_RGB_to_SEMI_PLANAR_YUV =
  { NULL,
    DEFAULT_UNIFORMS RGB_TO_YUV_COEFFICIENTS "uniform sampler2D tex;\n"
    "uniform vec2 chroma_sampling;\n",
    {glsl_func_swizzle, glsl_func_color_matrix, NULL, },
    GST_GL_TEXTURE_TARGET_2D
  };

/* YUY2:r,g,a
   UYVY:a,b,r */
static const gchar glsl_func_YUY2_UYVY_unpack[] =
    "vec4 yuy2_uyvy_unpack(sampler2D tex, vec2 v_texcoord, vec2 vert_to_tex, vec2 chroma_sampling) {\n"
    "  vec4 yuva;\n"
    "  float dx1 = -poffset_x;\n"
    "  float dx2 = 0.0;\n"
    "  yuva.x = texture2D(tex, v_texcoord * vert_to_tex * tex_scale0)[input_swizzle[0]];\n"
    /* v_texcoord are normalized, texcoord may not be e.g. rectangle textures */
    "  vec2 half_poffset = vec2(poffset_x / 2.0, poffset_y / 2.0);\n"
    "  int inorder = int(((v_texcoord.x * vert_to_tex.x - half_poffset.x) * chroma_sampling.x + half_poffset.x) * width / vert_to_tex) % 2;\n"
    "  if (inorder == 0) {\n"
    "    dx2 = -dx1;\n"
    "    dx1 = 0.0;\n"
    "  }\n"
    "  vec2 non_offset = v_texcoord * vert_to_tex * tex_scale0 - half_poffset;\n"
    "  vec4 u_texel = texture2D(tex, non_offset * chroma_sampling + half_poffset + vec2(dx1, 0.0));\n"
    "  vec4 v_texel = texture2D(tex, non_offset * chroma_sampling + half_poffset + vec2(dx2, 0.0));\n"
    "  yuva.yz = vec2(u_texel[input_swizzle[1]], v_texel[input_swizzle[2]]);\n"
    "  yuva.a = 1.0;\n"
    "  return yuva;\n"
    "}\n";

static const gchar templ_YUY2_UYVY_to_RGB_BODY[] =
    "vec4 yuva = yuy2_uyvy_unpack(Ytex, v_texcoord, vert_to_tex, vec2(1.0));\n"
    "vec4 rgba = color_matrix_apply(yuva, to_RGB_matrix);\n"
    "gl_FragColor = swizzle(rgba, output_swizzle);\n";

static const struct shader_templ templ_YUY2_UYVY_to_RGB =
  { NULL,
    DEFAULT_UNIFORMS YUV_TO_RGB_COEFFICIENTS "uniform sampler2D Ytex;\n" "uniform vec2 vert_to_tex;\n",
    { glsl_func_swizzle, glsl_func_color_matrix, glsl_func_YUY2_UYVY_unpack, NULL, },
    GST_GL_TEXTURE_TARGET_2D
  };

static const gchar templ_YUY2_UYVY_to_PLANAR_YUV_BODY[] =
    "vec4 yuva = yuy2_uyvy_unpack(Ytex, v_texcoord, vert_to_tex, chroma_sampling);\n"
    "yuva = swizzle(yuva, output_swizzle) * out_bitdepth_factor;\n"
    "write_planar_yuv(yuva);\n";

static const struct shader_templ templ_YUY2_UYVY_to_PLANAR_YUV =
  { NULL,
    DEFAULT_UNIFORMS YUV_TO_RGB_COEFFICIENTS "uniform sampler2D Ytex;\n" "uniform vec2 chroma_sampling;\n" "uniform float out_bitdepth_factor;\n" "uniform vec2 vert_to_tex;\n",
    { glsl_func_swizzle, glsl_func_color_matrix, glsl_func_YUY2_UYVY_unpack, glsl_func_write_planar_yuv, NULL, },
    GST_GL_TEXTURE_TARGET_2D
  };

static const gchar templ_YUY2_UYVY_to_PLANAR_YUVA_BODY[] =
    "vec4 yuva = yuy2_uyvy_unpack(Ytex, v_texcoord, vert_to_tex, chroma_sampling);\n"
    "yuva = swizzle(yuva, output_swizzle) * out_bitdepth_factor;\n"
    "write_planar_yuva(yuva);\n";

static const struct shader_templ templ_YUY2_UYVY_to_PLANAR_YUVA =
  { NULL,
    DEFAULT_UNIFORMS YUV_TO_RGB_COEFFICIENTS "uniform sampler2D Ytex;\n" "uniform vec2 chroma_sampling;\n" "uniform float out_bitdepth_factor;\n" "uniform vec2 vert_to_tex;\n",
    { glsl_func_swizzle, glsl_func_color_matrix, glsl_func_YUY2_UYVY_unpack, glsl_func_write_planar_yuva, NULL, },
    GST_GL_TEXTURE_TARGET_2D
  };

static const gchar glsl_func_YUY2_UYVY_pack[] =
    "ivec2 YUY2_UYVY_pack(vec2 texcoord, vec2 v_texcoord, out vec2 texcoord0, out vec2 texcoord1) {\n"
    "  vec4 texel1, texel2;\n"
    "  vec4 yuva, yuv1, yuv2;\n"
    "  float fx, dx, fy;\n"
    /* v_texcoord are normalized, texcoord may not be e.g. rectangle textures */
    "  float inorder = mod (v_texcoord.x * out_width, 2.0);\n"
    "  fx = texcoord.x;\n"
    "  dx = poffset_x;\n"
    "  if (inorder > 1.0) {\n"
    "    dx = -dx;\n"
    "  }\n"
    "  fy = texcoord.y;\n"
    "  texcoord0 = vec2(fx, fy);\n"
    "  texcoord1 = vec2(fx + dx, fy);\n"
    "  if (inorder < 1.0) {\n"
    "    return ivec2(output_swizzle[0], output_swizzle[1]);\n"
    "  } else {\n"
    "    return ivec2(output_swizzle[2], output_swizzle[3]);\n"
    "  }\n"
    "}\n";

static const gchar templ_RGB_to_YUY2_UYVY_BODY[] =
    "vec2 texcoord0, texcoord1;\n"
    "ivec2 idx = YUY2_UYVY_pack(texcoord, v_texcoord, texcoord0, texcoord1);\n"
    "vec4 rgba0 = swizzle(texture2D(tex, texcoord0), input_swizzle);\n"
    "vec4 rgba1 = swizzle(texture2D(tex, texcoord1), input_swizzle);\n"
    "vec4 yuva0 = color_matrix_apply(rgba0, to_YUV_matrix);\n"
    "vec4 yuva1 = color_matrix_apply(rgba1, to_YUV_matrix);\n"
    "vec4 yuva;\n"
    "yuva.x = yuva0.x;\n"
    "yuva.yz = (yuva0.yz + yuva1.yz) * 0.5;\n"
    "gl_FragColor = vec4(yuva[idx[0]], yuva[idx[1]], 0.0, 0.0);\n";

static const struct shader_templ templ_RGB_to_YUY2_UYVY =
  { NULL,
    DEFAULT_UNIFORMS RGB_TO_YUV_COEFFICIENTS "uniform sampler2D tex;\n" "uniform float out_width;\n",
    { glsl_func_swizzle, glsl_func_color_matrix, glsl_func_YUY2_UYVY_pack, NULL, },
    GST_GL_TEXTURE_TARGET_2D
  };

static const gchar templ_PLANAR_YUV_to_YUY2_UYVY_BODY[] =
    "vec2 texcoord0, texcoord1;\n"
    "ivec2 idx = YUY2_UYVY_pack(texcoord, v_texcoord, texcoord0, texcoord1);\n"
    "vec4 yuva0 = fetch_planar_yuv(Ytex, Utex, Vtex, texcoord0);\n"
    "vec4 yuva1 = fetch_planar_yuv(Ytex, Utex, Vtex, texcoord1);\n"
    "vec4 yuva;\n"
    "yuva.x = yuva0.x;\n"
    "yuva.yz = (yuva0.yz + yuva1.yz) * 0.5;\n"
    "gl_FragColor = vec4(yuva[idx[0]], yuva[idx[1]], 0.0, 0.0);\n";

static const struct shader_templ templ_PLANAR_YUV_to_YUY2_UYVY =
  { NULL,
    DEFAULT_UNIFORMS RGB_TO_YUV_COEFFICIENTS "uniform sampler2D Ytex;\n" "uniform sampler2D Utex;\n" "uniform sampler2D Vtex;\n" "uniform float in_bitdepth_factor;\n" "uniform float out_width;\n",
    { glsl_func_swizzle, glsl_func_color_matrix, glsl_func_YUY2_UYVY_pack, glsl_func_fetch_planar_yuv, NULL, },
    GST_GL_TEXTURE_TARGET_2D
  };

/* v210 layout (produces 6 pixels):
 * U Y V X | Y U Y X | V Y U X | Y V Y X
 */

static const char glsl_func_v210_unpack[] =
    "ivec2 v210_component_to_texel(int comp) {\n"
    // (the texel index in the row, the swizzle index in that texel)
    "  return ivec2(comp / 3, comp % 3);\n"
    "}\n"
    "ivec2 v210_y_xoffset(int xpos) {\n"
    // 1 3 5 7 9 ...
    "  int texel_i = xpos * 2 + 1;\n"
    "  return v210_component_to_texel(texel_i);\n"
    "}\n"
    "ivec2 v210_u_xoffset(int xpos) {\n"
    // 0 0 4 4 8 8 12 12 ...
    "  int texel_i = 4 * (xpos / 2);\n"
    "  return v210_component_to_texel(texel_i);\n"
    "}\n"
    "ivec2 v210_v_xoffset(int xpos) {\n"
    // 2 2 6 6 10 10 14 14 ...
    "  int texel_i = 4 * (xpos / 2) + 2;\n"
    "  return v210_component_to_texel(texel_i);\n"
    "}\n"
    "vec4 v210_unpack(sampler2D tex, vec2 v_texcoord, vec2 vert_to_tex, vec2 chroma_sampling) {\n"
    "  int xpos = int(v_texcoord.x * out_width);\n"
    "  ivec2 y_xoffset = v210_y_xoffset(xpos);\n"
    "  ivec2 u_xoffset = v210_u_xoffset(xpos * int(chroma_sampling.x));\n"
    "  ivec2 v_xoffset = v210_v_xoffset(xpos * int(chroma_sampling.x));\n"
    "  vec2 half_x_offset = vec2(poffset_x / 2.0, 0.0);\n"
    "  vec4 y_texel = texture2D(tex, vec2(poffset_x * float(y_xoffset[0]), v_texcoord.y * vert_to_tex.y) * tex_scale0 + half_x_offset);\n"
    "  vec4 u_texel = texture2D(tex, vec2(poffset_x * float(u_xoffset[0]), v_texcoord.y * vert_to_tex.y * chroma_sampling.y) * tex_scale0 + half_x_offset);\n"
    "  vec4 v_texel = texture2D(tex, vec2(poffset_x * float(v_xoffset[0]), v_texcoord.y * vert_to_tex.y * chroma_sampling.y) * tex_scale0 + half_x_offset);\n"
    "  return vec4(y_texel[y_xoffset[1]], u_texel[u_xoffset[1]], v_texel[v_xoffset[1]], 1.0);\n"
    "}\n";

static const gchar templ_v210_to_RGB_BODY[] =
    "vec4 yuva = v210_unpack(Ytex, v_texcoord, vert_to_tex, vec2(1.0));\n"
    "vec4 rgba = color_matrix_apply(yuva, to_RGB_matrix);\n"
    "gl_FragColor = swizzle(rgba, output_swizzle);\n";

static const struct shader_templ templ_v210_to_RGB =
  { NULL,
    DEFAULT_UNIFORMS YUV_TO_RGB_COEFFICIENTS "uniform sampler2D Ytex;\n" "uniform float out_width;\n" "uniform vec2 vert_to_tex;\n",
    { glsl_func_swizzle, glsl_func_color_matrix, glsl_func_v210_unpack, NULL, },
    GST_GL_TEXTURE_TARGET_2D
  };

static const gchar templ_v210_to_PLANAR_YUV_BODY[] =
    "vec4 yuva = v210_unpack(Ytex, v_texcoord, vert_to_tex, chroma_sampling);\n"
    "yuva = swizzle(yuva, output_swizzle) * out_bitdepth_factor;\n"
    "write_planar_yuv(yuva);\n";

static const struct shader_templ templ_v210_to_PLANAR_YUV =
  { NULL,
    DEFAULT_UNIFORMS YUV_TO_RGB_COEFFICIENTS "uniform sampler2D Ytex;\n" "uniform float out_width;\n" "uniform float out_bitdepth_factor;\n" "uniform vec2 chroma_sampling;\n" "uniform vec2 vert_to_tex;\n",
    { glsl_func_swizzle, glsl_func_color_matrix, glsl_func_v210_unpack, glsl_func_write_planar_yuv, NULL, },
    GST_GL_TEXTURE_TARGET_2D
  };

static const gchar templ_v210_to_PLANAR_YUVA_BODY[] =
    "vec4 yuva = v210_unpack(Ytex, v_texcoord, vert_to_tex, chroma_sampling);\n"
    "yuva = swizzle(yuva, output_swizzle) * out_bitdepth_factor;\n"
    "write_planar_yuva(yuva);\n";

static const struct shader_templ templ_v210_to_PLANAR_YUVA =
  { NULL,
    DEFAULT_UNIFORMS YUV_TO_RGB_COEFFICIENTS "uniform sampler2D Ytex;\n" "uniform float out_width;\n" "uniform float out_bitdepth_factor;\n" "uniform vec2 chroma_sampling;\n" "uniform vec2 vert_to_tex;\n",
    { glsl_func_swizzle, glsl_func_color_matrix, glsl_func_v210_unpack, glsl_func_write_planar_yuva, NULL, },
    GST_GL_TEXTURE_TARGET_2D
  };

static const char glsl_func_v210_pack[] =
    // UYV YUY VYU YVY
    "ivec3 v210_pack(vec2 texcoord, vec2 vert_to_tex, out vec2 texcoord0, out vec2 texcoord1, out vec2 texcoord2) {\n"
    // (texel index, component of the texel)
    // array initialisation is not available in GLES2 so we construct it
    // manually.
    "  ivec2[12] block_indices;\n"
    "  block_indices[0] = ivec2(0, 1);\n"
    "  block_indices[1] = ivec2(0, 0);\n"
    "  block_indices[2] = ivec2(0, 2);\n"
    "  block_indices[3] = ivec2(1, 0);\n"
    "  block_indices[4] = ivec2(2, 1);\n"
    "  block_indices[5] = ivec2(2, 0);\n"
    "  block_indices[6] = ivec2(2, 2);\n"
    "  block_indices[7] = ivec2(3, 0);\n"
    "  block_indices[8] = ivec2(4, 1);\n"
    "  block_indices[9] = ivec2(4, 0);\n"
    "  block_indices[10] = ivec2(4, 2);\n"
    "  block_indices[11] = ivec2(5, 0);\n"
    // poffset_x is in display width coordinates, not data width which is a
    // factor of 2/3 (4/6) different
    "  vec2 half_x_offset = vec2(poffset_x * 0.66666 / 2.0, 0.0);\n"
    "  int xpos = int(texcoord.x * out_width);\n"
    "  ivec2 sub_idx0 = block_indices[(xpos % 4) * 3 + 0];\n"
    "  ivec2 sub_idx1 = block_indices[(xpos % 4) * 3 + 1];\n"
    "  ivec2 sub_idx2 = block_indices[(xpos % 4) * 3 + 2];\n"
    "  vec2 block_offset = vec2(float((xpos / 4) * 6) * poffset_x, texcoord.y * vert_to_tex.y) + half_x_offset;\n"
    "  texcoord0 = block_offset + vec2(float(sub_idx0[0]) * poffset_x, 0.0);\n"
    "  texcoord1 = block_offset + vec2(float(sub_idx1[0]) * poffset_x, 0.0);\n"
    "  texcoord2 = block_offset + vec2(float(sub_idx2[0]) * poffset_x, 0.0);\n"
    "  return ivec3(sub_idx0[1], sub_idx1[1], sub_idx2[1]);\n"
    "}\n";

static const gchar templ_PLANAR_YUV_to_v210_BODY[] =
    "vec2 texcoord0, texcoord1, texcoord2;\n"
    "ivec3 idx = v210_pack(v_texcoord, vert_to_tex, texcoord0, texcoord1, texcoord2);\n"
    "vec4 yuva0 = fetch_planar_yuv(Ytex, Utex, Vtex, texcoord0);\n"
    "vec4 yuva1 = fetch_planar_yuv(Ytex, Utex, Vtex, texcoord1);\n"
    "vec4 yuva2 = fetch_planar_yuv(Ytex, Utex, Vtex, texcoord2);\n"
    "gl_FragColor = vec4(yuva0[idx[0]], yuva1[idx[1]], yuva2[idx[2]], 1.0);\n";

static const struct shader_templ templ_PLANAR_YUV_to_v210 =
  { NULL,
    DEFAULT_UNIFORMS YUV_TO_RGB_COEFFICIENTS "uniform sampler2D Ytex;\n" "uniform sampler2D Utex;\n" "uniform sampler2D Vtex;\n" "uniform float out_width;\n" "uniform float in_bitdepth_factor;\n" "uniform vec2 chroma_sampling;\n" "uniform vec2 vert_to_tex;\n",
    { glsl_func_swizzle, glsl_func_color_matrix, glsl_func_fetch_planar_yuv, glsl_func_v210_pack, NULL, },
    GST_GL_TEXTURE_TARGET_2D
  };

static const gchar templ_RGB_to_v210_BODY[] =
    "vec2 texcoord0, texcoord1, texcoord2;\n"
    "ivec3 idx = v210_pack(v_texcoord, vert_to_tex, texcoord0, texcoord1, texcoord2);\n"
    "vec4 rgba0 = swizzle(texture2D(tex, texcoord0), input_swizzle);\n"
    "vec4 rgba1 = swizzle(texture2D(tex, texcoord1), input_swizzle);\n"
    "vec4 rgba2 = swizzle(texture2D(tex, texcoord2), input_swizzle);\n"
    "vec4 yuva0 = color_matrix_apply(rgba0, to_YUV_matrix);\n"
    "vec4 yuva1 = color_matrix_apply(rgba1, to_YUV_matrix);\n"
    "vec4 yuva2 = color_matrix_apply(rgba2, to_YUV_matrix);\n"
    "gl_FragColor = vec4(yuva0[idx[0]], yuva1[idx[1]], yuva2[idx[2]], 1.0);\n";

static const struct shader_templ templ_RGB_to_v210 =
  { NULL,
    DEFAULT_UNIFORMS RGB_TO_YUV_COEFFICIENTS "uniform sampler2D tex;\n" "uniform float out_width;\n" "uniform vec2 vert_to_tex;\n",
    { glsl_func_swizzle, glsl_func_color_matrix, glsl_func_v210_pack, NULL, },
    GST_GL_TEXTURE_TARGET_2D
  };

static const gchar templ_YUY2_UYVY_to_v210_BODY[] =
    "vec2 texcoord0, texcoord1, texcoord2;\n"
    "ivec3 idx = v210_pack(v_texcoord, vert_to_tex, texcoord0, texcoord1, texcoord2);\n"
    "vec4 yuva0 = yuy2_uyvy_unpack(tex, texcoord0 / vert_to_tex, vert_to_tex, vec2(1.0));\n"
    "vec4 yuva1 = yuy2_uyvy_unpack(tex, texcoord1 / vert_to_tex, vert_to_tex, vec2(1.0));\n"
    "vec4 yuva2 = yuy2_uyvy_unpack(tex, texcoord2 / vert_to_tex, vert_to_tex, vec2(1.0));\n"
    "gl_FragColor = vec4(yuva0[idx[0]], yuva1[idx[1]], yuva2[idx[2]], 1.0);\n";

static const struct shader_templ templ_YUY2_UYVY_to_v210 =
  { NULL,
    DEFAULT_UNIFORMS YUV_TO_RGB_COEFFICIENTS "uniform sampler2D tex;\n" "uniform float out_width;\n" "uniform vec2 vert_to_tex;\n",
    { glsl_func_swizzle, glsl_func_color_matrix, glsl_func_YUY2_UYVY_unpack, glsl_func_v210_pack, NULL, },
    GST_GL_TEXTURE_TARGET_2D
  };

static const gchar templ_v210_to_YUY2_UYVY_BODY[] =
    "vec4 yuva = v210_unpack(tex, v_texcoord, vert_to_tex, vec2(1.0));\n"
    "vec2 texcoord0, texcoord1;\n"
    "ivec2 idx = YUY2_UYVY_pack(texcoord, v_texcoord, texcoord0, texcoord1);\n"
    "gl_FragColor = vec4(yuva[idx[0]], yuva[idx[1]], 0.0, 0.0);\n";

static const struct shader_templ templ_v210_to_YUY2_UYVY =
  { NULL,
    DEFAULT_UNIFORMS YUV_TO_RGB_COEFFICIENTS "uniform sampler2D tex;\n" "uniform float out_width;\n" "uniform vec2 vert_to_tex;\n",
    { glsl_func_swizzle, glsl_func_color_matrix, glsl_func_v210_unpack, glsl_func_YUY2_UYVY_pack, NULL, },
    GST_GL_TEXTURE_TARGET_2D
  };

static const gchar templ_YUY2_UYVY_to_YUY2_UYVY_BODY[] =
    "vec4 yuva = yuy2_uyvy_unpack(tex, v_texcoord, vert_to_tex, vec2(1.0));\n"
    "vec2 texcoord0, texcoord1;\n"
    "ivec2 idx = YUY2_UYVY_pack(texcoord, v_texcoord, texcoord0, texcoord1);\n"
    "gl_FragColor = vec4(yuva[idx[0]], yuva[idx[1]], 0.0, 0.0);\n";

static const struct shader_templ templ_YUY2_UYVY_to_YUY2_UYVY =
  { NULL,
    DEFAULT_UNIFORMS YUV_TO_RGB_COEFFICIENTS "uniform sampler2D tex;\n" "uniform float out_width;\n" "uniform vec2 vert_to_tex;\n",
    { glsl_func_swizzle, glsl_func_color_matrix, glsl_func_YUY2_UYVY_unpack, glsl_func_YUY2_UYVY_pack, NULL, },
    GST_GL_TEXTURE_TARGET_2D
  };

/* PLANAR RGB to PACKED RGB conversion */
static const gchar templ_PLANAR_RGB_to_PACKED_RGB_BODY[] =
    "vec4 rgba;\n"
    "rgba.r = texture2D(Rtex, texcoord * tex_scale0).r;\n"
    "rgba.g = texture2D(Gtex, texcoord * tex_scale1).r;\n"
    "rgba.b = texture2D(Btex, texcoord * tex_scale2).r;\n"
    "%s\n" /* alpha channel */
    "rgba = swizzle(rgba, input_swizzle);\n"
    "gl_FragColor = swizzle(rgba, output_swizzle);\n";

static const struct shader_templ templ_PLANAR_RGB_to_PACKED_RGB =
  { NULL,
    DEFAULT_UNIFORMS "uniform sampler2D Rtex, Gtex, Btex, Atex;\n",
    { glsl_func_swizzle, },
    GST_GL_TEXTURE_TARGET_2D
  };

/* PACKED RGB to PLANAR RGB conversion */
static const gchar templ_PACKED_RGB_to_PLANAR_RGB_BODY[] =
    "vec4 rgba;\n"
    "rgba = swizzle(texture2D(tex, texcoord), input_swizzle);\n"
    "rgba = swizzle(rgba, output_swizzle);\n"
    "gl_FragData[0] = vec4(rgba.r, 0, 0, 1.0);\n"
    "gl_FragData[1] = vec4(rgba.g, 0, 0, 1.0);\n"
    "gl_FragData[2] = vec4(rgba.b, 0, 0, 1.0);\n"
    "%s\n";

static const struct shader_templ templ_PACKED_RGB_to_PLANAR_RGB =
  { NULL,
    DEFAULT_UNIFORMS "uniform sampler2D tex;\n",
    { glsl_func_swizzle, },
    GST_GL_TEXTURE_TARGET_2D
  };

/* PLANAR RGB to PLANAR RGB conversion */
static const gchar templ_PLANAR_RGB_to_PLANAR_RGB_BODY[] =
    "vec4 rgba;\n"
    "rgba.r = texture2D(Rtex, texcoord * tex_scale0).r;\n"
    "rgba.g = texture2D(Gtex, texcoord * tex_scale1).r;\n"
    "rgba.b = texture2D(Btex, texcoord * tex_scale2).r;\n"
    "%s\n" /* alpha channel */
    "rgba = swizzle(rgba, input_swizzle);\n"
    "rgba = swizzle(rgba, output_swizzle);\n"
    "gl_FragData[0] = vec4(rgba.r, 0, 0, 1.0);\n"
    "gl_FragData[1] = vec4(rgba.g, 0, 0, 1.0);\n"
    "gl_FragData[2] = vec4(rgba.b, 0, 0, 1.0);\n"
    "%s\n";

static const struct shader_templ templ_PLANAR_RGB_to_PLANAR_RGB =
  { NULL,
    DEFAULT_UNIFORMS "uniform sampler2D Rtex, Gtex, Btex, Atex;\n",
    { glsl_func_swizzle, },
    GST_GL_TEXTURE_TARGET_2D
  };

static const gchar text_vertex_shader[] =
    "attribute vec4 a_position;   \n"
    "attribute vec2 a_texcoord;   \n"
    "varying vec2 v_texcoord;     \n"
    "void main()                  \n"
    "{                            \n"
    "  gl_Position = a_position; \n"
    "  v_texcoord = a_texcoord;  \n"
    "}                            \n";

static const GLfloat vertices[] = {
     1.0f, -1.0f, 0.0f, 1.0f, 0.0f,
    -1.0f, -1.0f, 0.0f, 0.0f, 0.0f,
    -1.0f,  1.0f, 0.0f, 0.0f, 1.0f,
     1.0f,  1.0f, 0.0f, 1.0f, 1.0f
};

static const GLushort indices[] = { 0, 1, 2, 0, 2, 3 };

/* *INDENT-ON* */

typedef struct
{
  double dm[4][4];
} Matrix4;

static void
matrix_debug (const Matrix4 * s)
{
  GST_DEBUG ("[%f %f %f %f]", s->dm[0][0], s->dm[0][1], s->dm[0][2],
      s->dm[0][3]);
  GST_DEBUG ("[%f %f %f %f]", s->dm[1][0], s->dm[1][1], s->dm[1][2],
      s->dm[1][3]);
  GST_DEBUG ("[%f %f %f %f]", s->dm[2][0], s->dm[2][1], s->dm[2][2],
      s->dm[2][3]);
  GST_DEBUG ("[%f %f %f %f]", s->dm[3][0], s->dm[3][1], s->dm[3][2],
      s->dm[3][3]);
}

static void
matrix_to_float (const Matrix4 * m, float *ret)
{
  int i, j;

  for (i = 0; i < 4; i++) {
    for (j = 0; j < 4; j++) {
      ret[j * 4 + i] = m->dm[i][j];
    }
  }
}

static void
matrix_set_identity (Matrix4 * m)
{
  int i, j;

  for (i = 0; i < 4; i++) {
    for (j = 0; j < 4; j++) {
      m->dm[i][j] = (i == j);
    }
  }
}

static void
matrix_copy (Matrix4 * d, const Matrix4 * s)
{
  gint i, j;

  for (i = 0; i < 4; i++)
    for (j = 0; j < 4; j++)
      d->dm[i][j] = s->dm[i][j];
}

/* Perform 4x4 matrix multiplication:
 *  - @dst@ = @a@ * @b@
 *  - @dst@ may be a pointer to @a@ andor @b@
 */
static void
matrix_multiply (Matrix4 * dst, Matrix4 * a, Matrix4 * b)
{
  Matrix4 tmp;
  int i, j, k;

  for (i = 0; i < 4; i++) {
    for (j = 0; j < 4; j++) {
      double x = 0;
      for (k = 0; k < 4; k++) {
        x += a->dm[i][k] * b->dm[k][j];
      }
      tmp.dm[i][j] = x;
    }
  }
  matrix_copy (dst, &tmp);
}

#if 0
static void
matrix_invert (Matrix4 * d, Matrix4 * s)
{
  Matrix4 tmp;
  int i, j;
  double det;

  matrix_set_identity (&tmp);
  for (j = 0; j < 3; j++) {
    for (i = 0; i < 3; i++) {
      tmp.dm[j][i] =
          s->dm[(i + 1) % 3][(j + 1) % 3] * s->dm[(i + 2) % 3][(j + 2) % 3] -
          s->dm[(i + 1) % 3][(j + 2) % 3] * s->dm[(i + 2) % 3][(j + 1) % 3];
    }
  }
  det =
      tmp.dm[0][0] * s->dm[0][0] + tmp.dm[0][1] * s->dm[1][0] +
      tmp.dm[0][2] * s->dm[2][0];
  for (j = 0; j < 3; j++) {
    for (i = 0; i < 3; i++) {
      tmp.dm[i][j] /= det;
    }
  }
  matrix_copy (d, &tmp);
}
#endif
static void
matrix_offset_components (Matrix4 * m, double a1, double a2, double a3)
{
  Matrix4 a;

  matrix_set_identity (&a);
  a.dm[0][3] = a1;
  a.dm[1][3] = a2;
  a.dm[2][3] = a3;
  matrix_debug (&a);
  matrix_multiply (m, &a, m);
}

static void
matrix_scale_components (Matrix4 * m, double a1, double a2, double a3)
{
  Matrix4 a;

  matrix_set_identity (&a);
  a.dm[0][0] = a1;
  a.dm[1][1] = a2;
  a.dm[2][2] = a3;
  matrix_multiply (m, &a, m);
}

struct ConvertInfo
{
  gint in_n_textures;
  gint out_n_textures;
  const struct shader_templ *templ;
  gchar *frag_body;
  gchar *frag_prog;
  const gchar *shader_tex_names[GST_VIDEO_MAX_PLANES];
  Matrix4 to_RGB_matrix;
  Matrix4 to_YUV_matrix;
  gfloat chroma_sampling[2];
  int input_swizzle[GST_VIDEO_MAX_PLANES];
  int output_swizzle[GST_VIDEO_MAX_PLANES];
  gfloat in_bitdepth_factor;
  gfloat out_bitdepth_factor;
};

static void
matrix_YCbCr_to_RGB (Matrix4 * m, double Kr, double Kb)
{
  double Kg = 1.0 - Kr - Kb;
  Matrix4 k = {
    {
          {1., 0., 2 * (1 - Kr), 0.},
          {1., -2 * Kb * (1 - Kb) / Kg, -2 * Kr * (1 - Kr) / Kg, 0.},
          {1., 2 * (1 - Kb), 0., 0.},
          {0., 0., 0., 1.},
        }
  };

  matrix_multiply (m, &k, m);
}

static void
convert_to_RGB (struct ConvertInfo *conv, GstVideoInfo * info)
{
  Matrix4 *m = &conv->to_RGB_matrix;

  {
    const GstVideoFormatInfo *uinfo;
    gint offset[4], scale[4], depth[4];
    int i;

    uinfo = gst_video_format_get_info (GST_VIDEO_INFO_FORMAT (info));

    /* bring color components to [0..1.0] range */
    gst_video_color_range_offsets (info->colorimetry.range, uinfo, offset,
        scale);

    for (i = 0; i < uinfo->n_components; i++)
      depth[i] = (1 << uinfo->depth[i]) - 1;

    matrix_offset_components (m, -offset[0] / (float) depth[0],
        -offset[1] / (float) depth[1], -offset[2] / (float) depth[2]);
    matrix_scale_components (m, depth[0] / ((float) scale[0]),
        depth[1] / ((float) scale[1]), depth[2] / ((float) scale[2]));
    GST_DEBUG ("to RGB scale/offset matrix");
    matrix_debug (m);
  }

  if (GST_VIDEO_INFO_IS_YUV (info)) {
    gdouble Kr, Kb;

    if (gst_video_color_matrix_get_Kr_Kb (info->colorimetry.matrix, &Kr, &Kb))
      matrix_YCbCr_to_RGB (m, Kr, Kb);
    GST_DEBUG ("to RGB matrix");
    matrix_debug (m);
  }
}

static void
matrix_RGB_to_YCbCr (Matrix4 * m, double Kr, double Kb)
{
  double Kg = 1.0 - Kr - Kb;
  Matrix4 k;
  double x;

  k.dm[0][0] = Kr;
  k.dm[0][1] = Kg;
  k.dm[0][2] = Kb;
  k.dm[0][3] = 0;

  x = 1 / (2 * (1 - Kb));
  k.dm[1][0] = -x * Kr;
  k.dm[1][1] = -x * Kg;
  k.dm[1][2] = x * (1 - Kb);
  k.dm[1][3] = 0;

  x = 1 / (2 * (1 - Kr));
  k.dm[2][0] = x * (1 - Kr);
  k.dm[2][1] = -x * Kg;
  k.dm[2][2] = -x * Kb;
  k.dm[2][3] = 0;

  k.dm[3][0] = 0;
  k.dm[3][1] = 0;
  k.dm[3][2] = 0;
  k.dm[3][3] = 1;

  matrix_multiply (m, &k, m);
}

static void
convert_to_YUV (struct ConvertInfo *conv, GstVideoInfo * info)
{
  Matrix4 *m = &conv->to_YUV_matrix;

  if (GST_VIDEO_INFO_IS_YUV (info)) {
    gdouble Kr, Kb;

    if (gst_video_color_matrix_get_Kr_Kb (info->colorimetry.matrix, &Kr, &Kb))
      matrix_RGB_to_YCbCr (m, Kr, Kb);
    GST_DEBUG ("to YUV matrix");
    matrix_debug (m);
  }

  {
    const GstVideoFormatInfo *uinfo;
    gint offset[4], scale[4], depth[4];
    int i;

    uinfo = gst_video_format_get_info (GST_VIDEO_INFO_FORMAT (info));

    /* bring color components to nominal range */
    gst_video_color_range_offsets (info->colorimetry.range, uinfo, offset,
        scale);

    for (i = 0; i < uinfo->n_components; i++)
      depth[i] = (1 << uinfo->depth[i]) - 1;

    matrix_scale_components (m, scale[0] / (float) depth[0],
        scale[1] / (float) depth[1], scale[2] / (float) depth[2]);
    matrix_offset_components (m, offset[0] / (float) depth[0],
        offset[1] / (float) depth[1], offset[2] / (float) depth[2]);
    GST_DEBUG ("to YUV scale/offset matrix");
    matrix_debug (m);
  }
}

struct _GstGLColorConvertPrivate
{
  gboolean result;

  struct ConvertInfo convert_info;

  GstGLTextureTarget from_texture_target;
  GstGLTextureTarget to_texture_target;

  GstGLMemory *in_tex[GST_VIDEO_MAX_PLANES];
  GstGLMemory *out_tex[GST_VIDEO_MAX_PLANES];

  GstGLFormat in_tex_formats[GST_VIDEO_MAX_PLANES];

  GLuint vao;
  GLuint vertex_buffer;
  GLuint vbo_indices;
  GLuint attr_position;
  GLuint attr_texture;

  GstCaps *in_caps;
  GstCaps *out_caps;

  GstBufferPool *pool;
  gboolean pool_started;
};

GST_DEBUG_CATEGORY_STATIC (gst_gl_color_convert_debug);
#define GST_CAT_DEFAULT gst_gl_color_convert_debug

#define DEBUG_INIT \
  GST_DEBUG_CATEGORY_INIT (gst_gl_color_convert_debug, "glconvert", 0, "convert");

G_DEFINE_TYPE_WITH_CODE (GstGLColorConvert, gst_gl_color_convert,
    GST_TYPE_OBJECT, G_ADD_PRIVATE (GstGLColorConvert) DEBUG_INIT);

static void gst_gl_color_convert_finalize (GObject * object);
static void gst_gl_color_convert_reset (GstGLColorConvert * convert);

#define GST_GL_COLOR_CONVERT_GET_PRIVATE(obj) (G_TYPE_INSTANCE_GET_PRIVATE ((obj), \
    GST_TYPE_GL_COLOR_CONVERT, GstGLColorConvertPrivate))

static void
gst_gl_color_convert_class_init (GstGLColorConvertClass * klass)
{
  G_OBJECT_CLASS (klass)->finalize = gst_gl_color_convert_finalize;
}

static void
gst_gl_color_convert_init (GstGLColorConvert * convert)
{
  convert->priv = gst_gl_color_convert_get_instance_private (convert);

  gst_gl_color_convert_reset (convert);
}

/**
 * gst_gl_color_convert_new:
 * @context: a #GstGLContext
 *
 * Returns: (transfer full): a new #GstGLColorConvert object
 *
 * Since: 1.4
 */
GstGLColorConvert *
gst_gl_color_convert_new (GstGLContext * context)
{
  GstGLColorConvert *convert;

  convert = g_object_new (GST_TYPE_GL_COLOR_CONVERT, NULL);
  gst_object_ref_sink (convert);

  convert->context = gst_object_ref (context);

  gst_video_info_set_format (&convert->in_info, GST_VIDEO_FORMAT_ENCODED, 0, 0);
  gst_video_info_set_format (&convert->out_info, GST_VIDEO_FORMAT_ENCODED, 0,
      0);

  GST_DEBUG_OBJECT (convert,
      "Created new colorconvert for context %" GST_PTR_FORMAT, context);

  return convert;
}

static void
gst_gl_color_convert_finalize (GObject * object)
{
  GstGLColorConvert *convert;

  convert = GST_GL_COLOR_CONVERT (object);

  gst_gl_color_convert_reset (convert);

  if (convert->context) {
    gst_object_unref (convert->context);
    convert->context = NULL;
  }

  G_OBJECT_CLASS (gst_gl_color_convert_parent_class)->finalize (object);
}

static void
_reset_gl (GstGLContext * context, GstGLColorConvert * convert)
{
  const GstGLFuncs *gl = context->gl_vtable;

  if (convert->priv->vao) {
    gl->DeleteVertexArrays (1, &convert->priv->vao);
    convert->priv->vao = 0;
  }

  if (convert->priv->vertex_buffer) {
    gl->DeleteBuffers (1, &convert->priv->vertex_buffer);
    convert->priv->vertex_buffer = 0;
  }

  if (convert->priv->vbo_indices) {
    gl->DeleteBuffers (1, &convert->priv->vbo_indices);
    convert->priv->vbo_indices = 0;
  }
}

static void
gst_gl_color_convert_reset_shader (GstGLColorConvert * convert)
{
  convert->priv->convert_info.chroma_sampling[0] = 1.0f;
  convert->priv->convert_info.chroma_sampling[1] = 1.0f;

  if (convert->priv->convert_info.frag_prog) {
    g_free (convert->priv->convert_info.frag_prog);
    convert->priv->convert_info.frag_prog = NULL;
  }
  if (convert->priv->convert_info.frag_body) {
    g_free (convert->priv->convert_info.frag_body);
    convert->priv->convert_info.frag_body = NULL;
  }
  if (convert->shader) {
    gst_object_unref (convert->shader);
    convert->shader = NULL;
  }

  convert->initted = FALSE;
}

static void
gst_gl_color_convert_reset (GstGLColorConvert * convert)
{
  guint i;

  if (convert->fbo) {
    gst_object_unref (convert->fbo);
    convert->fbo = NULL;
  }

  for (i = 0; i < convert->priv->convert_info.out_n_textures; i++) {
    if (convert->priv->out_tex[i])
      gst_memory_unref ((GstMemory *) convert->priv->out_tex[i]);
    convert->priv->out_tex[i] = NULL;
  }

  if (convert->priv->pool) {
    convert->priv->pool_started = FALSE;

    gst_object_unref (convert->priv->pool);
    convert->priv->pool = NULL;
  }

  gst_caps_replace (&convert->priv->in_caps, NULL);
  gst_caps_replace (&convert->priv->out_caps, NULL);

  if (convert->context) {
    gst_gl_context_thread_add (convert->context,
        (GstGLContextThreadFunc) _reset_gl, convert);
  }

  gst_gl_color_convert_reset_shader (convert);
}

static gboolean
_gst_gl_color_convert_can_passthrough_info (const GstVideoInfo * in,
    const GstVideoInfo * out)
{
  gint i;

  if (GST_VIDEO_INFO_FORMAT (in) != GST_VIDEO_INFO_FORMAT (out))
    return FALSE;
  if (GST_VIDEO_INFO_WIDTH (in) != GST_VIDEO_INFO_WIDTH (out))
    return FALSE;
  if (GST_VIDEO_INFO_HEIGHT (in) != GST_VIDEO_INFO_HEIGHT (out))
    return FALSE;
  if (GST_VIDEO_INFO_SIZE (in) != GST_VIDEO_INFO_SIZE (out))
    return FALSE;

  for (i = 0; i < in->finfo->n_planes; i++) {
    if (in->stride[i] != out->stride[i])
      return FALSE;
    if (in->offset[i] != out->offset[i])
      return FALSE;
  }

  if (!gst_video_colorimetry_is_equal (&in->colorimetry, &out->colorimetry))
    return FALSE;
  if (in->chroma_site != out->chroma_site)
    return FALSE;

  return TRUE;
}

static gboolean
supports_yuv_yuv_conversion (const GstVideoFormatInfo * from)
{
  if (GST_VIDEO_FORMAT_INFO_IS_YUV (from)
      && GST_VIDEO_FORMAT_INFO_N_PLANES (from) ==
      GST_VIDEO_FORMAT_INFO_N_COMPONENTS (from))
    return TRUE;

  if (GST_VIDEO_FORMAT_INFO_FORMAT (from) == GST_VIDEO_FORMAT_v210)
    return TRUE;
  if (GST_VIDEO_FORMAT_INFO_FORMAT (from) == GST_VIDEO_FORMAT_UYVY)
    return TRUE;
  if (GST_VIDEO_FORMAT_INFO_FORMAT (from) == GST_VIDEO_FORMAT_YUY2)
    return TRUE;

  return FALSE;
}

static gboolean
conversion_formats_are_supported (const GstVideoFormatInfo * in_finfo,
    const GstVideoFormatInfo * out_finfo)
{
  if (GST_VIDEO_FORMAT_INFO_IS_RGB (in_finfo))
    return TRUE;
  if (GST_VIDEO_FORMAT_INFO_IS_RGB (out_finfo))
    return TRUE;

  if (supports_yuv_yuv_conversion (in_finfo)
      && supports_yuv_yuv_conversion (out_finfo))
    return TRUE;

  return FALSE;
}

static gboolean
_gst_gl_color_convert_set_caps_unlocked (GstGLColorConvert * convert,
    GstCaps * in_caps, GstCaps * out_caps)
{
  GstVideoInfo in_info, out_info;
  GstCapsFeatures *in_features, *out_features;
  GstGLTextureTarget from_target, to_target;
  gboolean passthrough;

  g_return_val_if_fail (convert != NULL, FALSE);
  g_return_val_if_fail (in_caps, FALSE);
  g_return_val_if_fail (out_caps, FALSE);

  GST_LOG_OBJECT (convert, "Setting caps in %" GST_PTR_FORMAT
      " out %" GST_PTR_FORMAT, in_caps, out_caps);

  if (!gst_video_info_from_caps (&in_info, in_caps))
    g_assert_not_reached ();

  if (!gst_video_info_from_caps (&out_info, out_caps))
    g_assert_not_reached ();

  g_return_val_if_fail (GST_VIDEO_INFO_FORMAT (&in_info) !=
      GST_VIDEO_FORMAT_UNKNOWN, FALSE);
  g_return_val_if_fail (GST_VIDEO_INFO_FORMAT (&in_info) !=
      GST_VIDEO_FORMAT_ENCODED, FALSE);
  g_return_val_if_fail (GST_VIDEO_INFO_FORMAT (&out_info) !=
      GST_VIDEO_FORMAT_UNKNOWN, FALSE);
  g_return_val_if_fail (GST_VIDEO_INFO_FORMAT (&out_info) !=
      GST_VIDEO_FORMAT_ENCODED, FALSE);

  in_features = gst_caps_get_features (in_caps, 0);
  out_features = gst_caps_get_features (out_caps, 0);

  if (!gst_caps_features_contains (in_features,
          GST_CAPS_FEATURE_MEMORY_GL_MEMORY)
      || !gst_caps_features_contains (out_features,
          GST_CAPS_FEATURE_MEMORY_GL_MEMORY)) {
    return FALSE;
  }

  {
    GstStructure *in_s = gst_caps_get_structure (in_caps, 0);
    GstStructure *out_s = gst_caps_get_structure (out_caps, 0);

    if (gst_structure_has_field_typed (in_s, "texture-target", G_TYPE_STRING))
      from_target =
          gst_gl_texture_target_from_string (gst_structure_get_string (in_s,
              "texture-target"));
    else
      from_target = GST_GL_TEXTURE_TARGET_2D;

    if (gst_structure_has_field_typed (out_s, "texture-target", G_TYPE_STRING))
      to_target =
          gst_gl_texture_target_from_string (gst_structure_get_string (out_s,
              "texture-target"));
    else
      to_target = GST_GL_TEXTURE_TARGET_2D;

    if (to_target == GST_GL_TEXTURE_TARGET_NONE
        || from_target == GST_GL_TEXTURE_TARGET_NONE)
      /* invalid caps */
      return FALSE;
  }

  if (gst_video_info_is_equal (&convert->in_info, &in_info) &&
      gst_video_info_is_equal (&convert->out_info, &out_info) &&
      convert->priv->from_texture_target == from_target &&
      convert->priv->to_texture_target == to_target)
    return TRUE;

  /* If input and output are identical, pass through directly */
  passthrough =
      _gst_gl_color_convert_can_passthrough_info (&in_info, &out_info) &&
      from_target == to_target;

  if (!passthrough && to_target != GST_GL_TEXTURE_TARGET_2D
      && to_target != GST_GL_TEXTURE_TARGET_RECTANGLE)
    return FALSE;

  if (!passthrough &&
      !conversion_formats_are_supported (in_info.finfo, out_info.finfo))
    return FALSE;

  gst_gl_color_convert_reset (convert);
  convert->in_info = in_info;
  convert->out_info = out_info;
  gst_caps_replace (&convert->priv->in_caps, in_caps);
  gst_caps_replace (&convert->priv->out_caps, out_caps);
  convert->priv->from_texture_target = from_target;
  convert->priv->to_texture_target = to_target;
  convert->initted = FALSE;

  convert->passthrough = passthrough;
#ifndef GST_DISABLE_GST_DEBUG
  if (G_UNLIKELY (convert->passthrough))
    GST_DEBUG_OBJECT (convert,
        "Configuring passthrough mode for same in/out caps");
  else {
    GST_DEBUG_OBJECT (convert, "Color converting %" GST_PTR_FORMAT
        " to %" GST_PTR_FORMAT, in_caps, out_caps);
  }
#endif

  return TRUE;
}

/**
 * gst_gl_color_convert_set_caps:
 * @convert: a #GstGLColorConvert
 * @in_caps: input #GstCaps
 * @out_caps: output #GstCaps
 *
 * Initializes @convert with the information required for conversion.
 *
 * Since: 1.6
 */
gboolean
gst_gl_color_convert_set_caps (GstGLColorConvert * convert,
    GstCaps * in_caps, GstCaps * out_caps)
{
  gboolean ret;

  GST_OBJECT_LOCK (convert);
  ret = _gst_gl_color_convert_set_caps_unlocked (convert, in_caps, out_caps);
  GST_OBJECT_UNLOCK (convert);

  return ret;
}

/**
 * gst_gl_color_convert_decide_allocation:
 * @convert: a #GstGLColorConvert
 * @query: a completed ALLOCATION #GstQuery
 *
 * Provides an implementation of #GstBaseTransformClass.decide_allocation()
 *
 * Returns: whether the allocation parameters were successfully chosen
 *
 * Since: 1.8
 */
gboolean
gst_gl_color_convert_decide_allocation (GstGLColorConvert * convert,
    GstQuery * query)
{
  GstBufferPool *pool = NULL;
  GstStructure *config;
  GstCaps *caps;
  guint min, max, size, n, i;
  gboolean update_pool;
  GstGLVideoAllocationParams *params;
  GstVideoInfo vinfo;

  gst_query_parse_allocation (query, &caps, NULL);
  if (!caps)
    return FALSE;

  gst_video_info_from_caps (&vinfo, caps);

  n = gst_query_get_n_allocation_pools (query);
  if (n > 0) {
    update_pool = TRUE;
    for (i = 0; i < n; i++) {
      gst_query_parse_nth_allocation_pool (query, i, &pool, &size, &min, &max);

      if (!pool || !GST_IS_GL_BUFFER_POOL (pool)) {
        if (pool)
          gst_object_unref (pool);
        pool = NULL;
      }
    }
  }

  if (!pool) {
    GstVideoInfo vinfo;

    gst_video_info_init (&vinfo);
    size = vinfo.size;
    min = max = 0;
    update_pool = FALSE;
  }

  if (!pool)
    pool = gst_gl_buffer_pool_new (convert->context);

  config = gst_buffer_pool_get_config (pool);

  gst_buffer_pool_config_set_params (config, caps, size, min, max);
  gst_buffer_pool_config_add_option (config, GST_BUFFER_POOL_OPTION_VIDEO_META);
  if (gst_query_find_allocation_meta (query, GST_GL_SYNC_META_API_TYPE, NULL))
    gst_buffer_pool_config_add_option (config,
        GST_BUFFER_POOL_OPTION_GL_SYNC_META);

  params = gst_gl_video_allocation_params_new (convert->context, NULL, &vinfo,
      0, NULL, convert->priv->to_texture_target, 0);
  gst_buffer_pool_config_set_gl_allocation_params (config,
      (GstGLAllocationParams *) params);
  gst_gl_allocation_params_free ((GstGLAllocationParams *) params);

  if (!gst_buffer_pool_set_config (pool, config))
    GST_WARNING_OBJECT (convert, "Failed to set buffer pool config");

  if (update_pool)
    gst_query_set_nth_allocation_pool (query, 0, pool, size, min, max);
  else
    gst_query_add_allocation_pool (query, pool, size, min, max);

  if (convert->priv->pool) {
    gst_object_unref (convert->priv->pool);
    convert->priv->pool_started = FALSE;
  }
  convert->priv->pool = pool;

  return TRUE;
}

static void
_init_value_string_list (GValue * list, ...)
{
  GValue item = G_VALUE_INIT;
  gchar *str;
  va_list args;

  g_value_init (list, GST_TYPE_LIST);

  va_start (args, list);
  while ((str = va_arg (args, gchar *))) {
    g_value_init (&item, G_TYPE_STRING);
    g_value_set_string (&item, str);

    gst_value_list_append_value (list, &item);
    g_value_unset (&item);
  }
  va_end (args);
}

static void
_append_value_string_list (GValue * list, ...)
{
  GValue item = G_VALUE_INIT;
  gchar *str;
  va_list args;

  va_start (args, list);
  while ((str = va_arg (args, gchar *))) {
    g_value_init (&item, G_TYPE_STRING);
    g_value_set_string (&item, str);

    gst_value_list_append_value (list, &item);
    g_value_unset (&item);
  }
  va_end (args);
}

static void
_init_supported_formats (GstGLContext * context, gboolean output,
    GValue * supported_formats)
{
  /* Assume if context == NULL that we don't have a GL context and can
   * do the conversion */

  /* Always supported input and output formats */
  _init_value_string_list (supported_formats, "RGBA", "RGB", "RGBx", "BGR",
      "BGRx", "BGRA", "xRGB", "xBGR", "ARGB", "ABGR", "GRAY8", "GRAY16_LE",
      "GRAY16_BE", "AYUV", "VUYA", "YUY2", "UYVY", "RBGA", NULL);

  /* Always supported input formats or output with multiple draw buffers */
  if (!output || (!context || context->gl_vtable->DrawBuffers))
    _append_value_string_list (supported_formats, "GBRA", "GBR", "RGBP", "BGRP",
        "Y444", "I420", "YV12", "Y42B", "Y41B", "NV12", "NV21", "NV16", "NV61",
        "A420", "AV12", "A444", "A422", NULL);

  /* Requires reading from a RG/LA framebuffer... */
  if (!context || (USING_GLES3 (context) || USING_OPENGL (context)))
    _append_value_string_list (supported_formats, "YUY2", "UYVY", NULL);

  if (!context || gst_gl_format_is_supported (context, GST_GL_RGBA16)) {
    _append_value_string_list (supported_formats, "ARGB64", NULL);
#if G_BYTE_ORDER == G_LITTLE_ENDIAN
    _append_value_string_list (supported_formats, "RGBA64_LE", NULL);
#else
    _append_value_string_list (supported_formats, "RGBA64_BE", NULL);
#endif
  }

  if (!context || gst_gl_format_is_supported (context, GST_GL_RGB565))
    _append_value_string_list (supported_formats, "RGB16", "BGR16", NULL);

  if (!context || gst_gl_format_is_supported (context, GST_GL_RGB10_A2)) {
#if G_BYTE_ORDER == G_LITTLE_ENDIAN
    _append_value_string_list (supported_formats, "BGR10A2_LE", "RGB10A2_LE",
        "BGR10x2_LE", "RGB10x2_LE", "Y410", "v210", NULL);
#else
    _append_value_string_list (supported_formats, "Y410", NULL);
#endif
  }

  if (!context || (gst_gl_format_is_supported (context, GST_GL_R16) &&
          gst_gl_format_is_supported (context, GST_GL_RG16))) {
#if G_BYTE_ORDER == G_LITTLE_ENDIAN
    _append_value_string_list (supported_formats, "P010_10LE", "P012_LE",
        "P016_LE", NULL);
#else
    _append_value_string_list (supported_formats, "P010_10BE", "P012_BE",
        "P016_BE", NULL);
#endif
  }

  if (!context || (gst_gl_format_is_supported (context, GST_GL_RG16))) {
    _append_value_string_list (supported_formats, "Y210", NULL);
#if G_BYTE_ORDER == G_LITTLE_ENDIAN
    _append_value_string_list (supported_formats, "Y212_LE", NULL);
#else
    _append_value_string_list (supported_formats, "Y212_BE", NULL);
#endif
  }

  if (!context || gst_gl_format_is_supported (context, GST_GL_RGBA16)) {
#if G_BYTE_ORDER == G_LITTLE_ENDIAN
    _append_value_string_list (supported_formats, "Y412_LE", NULL);
#else
    _append_value_string_list (supported_formats, "Y412_BE", NULL);
#endif
  }

  if (!context || gst_gl_format_is_supported (context, GST_GL_R16)) {
#if G_BYTE_ORDER == G_LITTLE_ENDIAN
    _append_value_string_list (supported_formats, "A420_10LE", "A422_10LE",
        "A444_10LE", "A444_12LE", "A422_12LE", "A420_12LE", "A444_16LE",
        "A422_16LE", "A420_16LE", "I420_12LE", "I420_10LE", "I422_10LE",
        "I422_12LE", "Y444_10LE", "Y444_16LE", NULL);
#else
    _append_value_string_list (supported_formats, "A420_10BE", "A422_10BE",
        "A444_10BE", "A444_12BE", "A422_12BE", "A420_12BE", "A444_16BE",
        "A422_16BE", "A420_16BE", "I420_12BE", "I420_10BE", "I422_10BE",
        "I422_12BE", "Y444_10BE", "Y444_16BE", NULL);
#endif
  }

  if (!context || USING_GLES3 (context) || USING_OPENGL30 (context)) {
    _append_value_string_list (supported_formats, "NV12_16L32S", "NV12_4L4",
        NULL);
  }
}

/* copies the given caps */
static GstCaps *
gst_gl_color_convert_caps_transform_format_info (GstGLContext * context,
    gboolean output, GstCaps * caps)
{
  GstStructure *st;
  GstCapsFeatures *f;
  gint i, n;
  GstCaps *res;
  GValue supported_formats = G_VALUE_INIT;
  GValue rgb_formats = G_VALUE_INIT;
  GValue supported_rgb_formats = G_VALUE_INIT;
  GValue planar_yuv_formats = G_VALUE_INIT;
  GValue supported_planar_yuv_formats = G_VALUE_INIT;

  /* There are effectively two modes here with the RGB/YUV transition:
   * 1. There is a RGB-like format as input and we can transform to YUV or,
   * 2. No RGB-like format as input so we can only transform to RGB-like formats
   *
   * We also filter down the list of formats depending on what the OpenGL
   * context supports (when provided).
   */

  _init_value_string_list (&rgb_formats, "RGBA", "ARGB", "BGRA", "ABGR", "RGBx",
      "xRGB", "BGRx", "xBGR", "RGB", "BGR", "ARGB64", "BGR10A2_LE",
      "RGB10A2_LE", "BGR10x2_LE", "RGB10x2_LE", "RGBA64_LE", "RGBA64_BE",
      "RBGA", "GBRA", "GBR", "RGBP", "BGRP", "RGB16", "BGR16", NULL);
  _init_value_string_list (&planar_yuv_formats, "Y444", "Y444_10LE",
      "Y444_16LE", "Y444_10BE", "Y444_16BE", "I420", "Y42B", "Y41B", "A420",
      "A444", "A422", "A420_10LE", "A422_10LE", "A444_10LE", "A444_12LE",
      "A422_12LE", "A420_12LE", "A444_16LE", "A422_16LE", "A420_16LE",
      "I420_12LE", "I420_10LE", "I422_10LE", "I422_12LE", "A420_10BE",
      "A422_10BE", "A444_10BE", "A444_12BE", "A422_12BE", "A420_12BE",
      "A444_16BE", "A422_16BE", "A420_16BE", "I420_12BE", "I420_10BE",
      "I422_10BE", "I422_12BE", "v210", "UYVY", "YUY2", NULL);
  _init_supported_formats (context, output, &supported_formats);
  gst_value_intersect (&supported_rgb_formats, &rgb_formats,
      &supported_formats);
  gst_value_intersect (&supported_planar_yuv_formats, &planar_yuv_formats,
      &supported_formats);

  res = gst_caps_new_empty ();

  n = gst_caps_get_size (caps);
  for (i = 0; i < n; i++) {
    const GValue *format;

    st = gst_caps_get_structure (caps, i);
    f = gst_caps_get_features (caps, i);

    format = gst_structure_get_value (st, "format");
    st = gst_structure_copy (st);

    /* Only remove format info for the cases when we can actually convert */
    if (!gst_caps_features_is_any (f) &&
        gst_caps_features_contains (f, GST_CAPS_FEATURE_MEMORY_GL_MEMORY)) {
      if (GST_VALUE_HOLDS_LIST (format)) {
        gboolean have_rgb_formats = FALSE;
        gboolean have_planar_yuv_formats = FALSE;
        GValue passthrough_formats = G_VALUE_INIT;
        gint j, len;

        g_value_init (&passthrough_formats, GST_TYPE_LIST);
        len = gst_value_list_get_size (format);
        for (j = 0; j < len; j++) {
          const GValue *val;

          val = gst_value_list_get_value (format, j);
          if (G_VALUE_HOLDS_STRING (val)) {
            const gchar *format_str = g_value_get_string (val);
            GstVideoFormat v_format = gst_video_format_from_string (format_str);
            const GstVideoFormatInfo *t_info =
                gst_video_format_get_info (v_format);
            if (GST_VIDEO_FORMAT_INFO_FLAGS (t_info) &
                (GST_VIDEO_FORMAT_FLAG_YUV | GST_VIDEO_FORMAT_FLAG_GRAY)) {
              gst_value_list_append_value (&passthrough_formats, val);
            } else if (GST_VIDEO_FORMAT_INFO_FLAGS (t_info) &
                GST_VIDEO_FORMAT_FLAG_RGB) {
              have_rgb_formats = TRUE;
              break;
            } else if (supports_yuv_yuv_conversion (t_info)) {
              have_planar_yuv_formats = TRUE;
            }
          }
        }
        if (have_rgb_formats) {
          gst_structure_set_value (st, "format", &supported_formats);
          gst_structure_remove_fields (st, "colorimetry", "chroma-site",
              "texture-target", NULL);
        } else {
          /* add passthrough structure */
          gst_structure_set_value (st, "format", &passthrough_formats);
          /* then optional planar yuv conversion structure */
          if (have_planar_yuv_formats) {
            gst_caps_append_structure_full (res, gst_structure_copy (st),
                gst_caps_features_copy (f));
            gst_structure_set_value (st, "format",
                &supported_planar_yuv_formats);
            gst_structure_remove_fields (st, "texture-target", NULL);
          }
          /* then the rgb conversion structure */
          gst_caps_append_structure_full (res, gst_structure_copy (st),
              gst_caps_features_copy (f));
          gst_structure_set_value (st, "format", &supported_rgb_formats);
          gst_structure_remove_fields (st, "colorimetry", "chroma-site",
              "texture-target", NULL);
        }
        g_value_unset (&passthrough_formats);
      } else if (G_VALUE_HOLDS_STRING (format)) {
        const gchar *format_str = g_value_get_string (format);
        GstVideoFormat v_format = gst_video_format_from_string (format_str);
        const GstVideoFormatInfo *t_info = gst_video_format_get_info (v_format);
        if (GST_VIDEO_FORMAT_INFO_IS_RGB (t_info)) {
          gst_structure_set_value (st, "format", &supported_formats);
          gst_structure_remove_fields (st, "colorimetry", "chroma-site",
              "texture-target", NULL);
        } else {
          /* add passthrough structure, then the rgb conversion structure */
          gst_structure_set_value (st, "format", format);
          if (supports_yuv_yuv_conversion (t_info)) {
            gst_caps_append_structure_full (res, gst_structure_copy (st),
                gst_caps_features_copy (f));
            gst_structure_set_value (st, "format",
                &supported_planar_yuv_formats);
            gst_structure_remove_fields (st, "texture-target", NULL);
          }
          gst_caps_append_structure_full (res, gst_structure_copy (st),
              gst_caps_features_copy (f));
          gst_structure_set_value (st, "format", &supported_rgb_formats);
          gst_structure_remove_fields (st, "colorimetry", "chroma-site",
              "texture-target", NULL);
        }
      }
    }

    gst_caps_append_structure_full (res, st, gst_caps_features_copy (f));
  }

  g_value_unset (&supported_formats);
  g_value_unset (&rgb_formats);
  g_value_unset (&supported_rgb_formats);
  g_value_unset (&planar_yuv_formats);
  g_value_unset (&supported_planar_yuv_formats);

  return res;
}

/**
 * gst_gl_color_convert_transform_caps:
 * @context: a #GstGLContext to use for transforming @caps
 * @direction: a #GstPadDirection
 * @caps: (transfer none): the #GstCaps to transform
 * @filter: (transfer none): a set of filter #GstCaps
 *
 * Provides an implementation of #GstBaseTransformClass.transform_caps()
 *
 * Returns: (transfer full): the converted #GstCaps
 *
 * Since: 1.6
 */
GstCaps *
gst_gl_color_convert_transform_caps (GstGLContext * context,
    GstPadDirection direction, GstCaps * caps, GstCaps * filter)
{
  caps = gst_gl_color_convert_caps_transform_format_info (context,
      direction == GST_PAD_SINK, caps);

  if (filter) {
    GstCaps *tmp;

    tmp = gst_caps_intersect_full (filter, caps, GST_CAPS_INTERSECT_FIRST);
    gst_caps_unref (caps);
    caps = tmp;
  }

  return caps;
}

/* fixation from videoconvert */
#define SCORE_FORMAT_CHANGE             1
#define SCORE_DEPTH_CHANGE              1
#define SCORE_ALPHA_CHANGE              1
#define SCORE_CHROMA_W_CHANGE           1
#define SCORE_CHROMA_H_CHANGE           1
#define SCORE_PALETTE_CHANGE            1

#define SCORE_COLORSPACE_LOSS           2       /* RGB <-> YUV */
#define SCORE_DEPTH_LOSS                4       /* change bit depth */
#define SCORE_ALPHA_LOSS                8       /* lose the alpha channel */
#define SCORE_CHROMA_W_LOSS             16      /* vertical subsample */
#define SCORE_CHROMA_H_LOSS             32      /* horizontal subsample */
#define SCORE_PALETTE_LOSS              64      /* convert to palette format */
#define SCORE_COLOR_LOSS                128     /* convert to GRAY */

#define COLORSPACE_MASK (GST_VIDEO_FORMAT_FLAG_YUV | \
                         GST_VIDEO_FORMAT_FLAG_RGB | GST_VIDEO_FORMAT_FLAG_GRAY)
#define ALPHA_MASK      (GST_VIDEO_FORMAT_FLAG_ALPHA)
#define PALETTE_MASK    (GST_VIDEO_FORMAT_FLAG_PALETTE)

static GstGLTextureTarget
_texture_target_demask (guint target_mask)
{
  if (target_mask & (1 << GST_GL_TEXTURE_TARGET_2D)) {
    return GST_GL_TEXTURE_TARGET_2D;
  }
  if (target_mask & (1 << GST_GL_TEXTURE_TARGET_RECTANGLE)) {
    return GST_GL_TEXTURE_TARGET_RECTANGLE;
  }
  if (target_mask & (1 << GST_GL_TEXTURE_TARGET_EXTERNAL_OES)) {
    return GST_GL_TEXTURE_TARGET_EXTERNAL_OES;
  }

  return 0;
}

/* calculate how much loss a conversion would be */
static void
score_format_target (const GstVideoFormatInfo * in_info, guint targets_mask,
    GstVideoFormat v_format, guint other_targets_mask, gint * min_loss,
    const GstVideoFormatInfo ** out_info, GstGLTextureTarget * result)
{
  const GstVideoFormatInfo *t_info;
  GstVideoFormatFlags in_flags, t_flags;
  gint loss;

  t_info = gst_video_format_get_info (v_format);
  if (!t_info)
    return;

  /* accept input format immediately without loss */
  if (in_info == t_info && (targets_mask & other_targets_mask) != 0) {
    *min_loss = 0;
    *out_info = t_info;
    *result = _texture_target_demask (targets_mask & other_targets_mask);
    return;
  }

  /* can only passthrough external-oes textures */
  other_targets_mask &= ~(1 << GST_GL_TEXTURE_TARGET_EXTERNAL_OES);
  /* try to keep the same target */
  if (targets_mask & other_targets_mask)
    other_targets_mask = targets_mask & other_targets_mask;

  loss = SCORE_FORMAT_CHANGE;

  in_flags = GST_VIDEO_FORMAT_INFO_FLAGS (in_info);
  in_flags &= ~GST_VIDEO_FORMAT_FLAG_LE;
  in_flags &= ~GST_VIDEO_FORMAT_FLAG_COMPLEX;
  in_flags &= ~GST_VIDEO_FORMAT_FLAG_UNPACK;

  t_flags = GST_VIDEO_FORMAT_INFO_FLAGS (t_info);
  t_flags &= ~GST_VIDEO_FORMAT_FLAG_LE;
  t_flags &= ~GST_VIDEO_FORMAT_FLAG_COMPLEX;
  t_flags &= ~GST_VIDEO_FORMAT_FLAG_UNPACK;

  if (!conversion_formats_are_supported (in_info, t_info))
    return;

  if ((t_flags & PALETTE_MASK) != (in_flags & PALETTE_MASK)) {
    loss += SCORE_PALETTE_CHANGE;
    if (t_flags & PALETTE_MASK)
      loss += SCORE_PALETTE_LOSS;
  }

  if ((t_flags & COLORSPACE_MASK) != (in_flags & COLORSPACE_MASK)) {
    loss += SCORE_COLORSPACE_LOSS;
    if (t_flags & GST_VIDEO_FORMAT_FLAG_GRAY)
      loss += SCORE_COLOR_LOSS;
  }

  if ((t_flags & ALPHA_MASK) != (in_flags & ALPHA_MASK)) {
    loss += SCORE_ALPHA_CHANGE;
    if (in_flags & ALPHA_MASK)
      loss += SCORE_ALPHA_LOSS;
  }

  if ((in_info->h_sub[1]) != (t_info->h_sub[1])) {
    loss += SCORE_CHROMA_H_CHANGE;
    if ((in_info->h_sub[1]) < (t_info->h_sub[1]))
      loss += SCORE_CHROMA_H_LOSS;
  }
  if ((in_info->w_sub[1]) != (t_info->w_sub[1])) {
    loss += SCORE_CHROMA_W_CHANGE;
    if ((in_info->w_sub[1]) < (t_info->w_sub[1]))
      loss += SCORE_CHROMA_W_LOSS;
  }

  if ((in_info->bits) != (t_info->bits)) {
    loss += SCORE_DEPTH_CHANGE;
    if ((in_info->bits) > (t_info->bits))
      loss += SCORE_DEPTH_LOSS;
  }

  if (loss < *min_loss) {
    GstGLTextureTarget target = _texture_target_demask (other_targets_mask);

    *out_info = t_info;
    *min_loss = loss;
    *result = target;
  }
}

static void
gst_gl_color_convert_fixate_format_target (GstCaps * caps, GstCaps * result)
{
  GstStructure *ins, *outs;
  const gchar *in_format;
  const GstVideoFormatInfo *in_info, *out_info = NULL;
  const GValue *targets;
  guint targets_mask = 0;
  GstGLTextureTarget target = GST_GL_TEXTURE_TARGET_NONE;
  gint min_loss = G_MAXINT;
  guint i, capslen;

  ins = gst_caps_get_structure (caps, 0);
  in_format = gst_structure_get_string (ins, "format");
  if (!in_format)
    return;
  targets = gst_structure_get_value (ins, "texture-target");
  if (targets)
    targets_mask = gst_gl_value_get_texture_target_mask (targets);

  in_info =
      gst_video_format_get_info (gst_video_format_from_string (in_format));
  if (!in_info)
    return;

  outs = gst_caps_get_structure (result, 0);

  capslen = gst_caps_get_size (result);
  for (i = 0; i < capslen; i++) {
    GstStructure *tests;
    const GValue *format;
    const GValue *other_targets;
    guint other_targets_mask = 0;

    tests = gst_caps_get_structure (result, i);

    format = gst_structure_get_value (tests, "format");
    /* should not happen */
    if (format == NULL)
      continue;

    other_targets = gst_structure_get_value (tests, "texture-target");
    if (other_targets)
      other_targets_mask = gst_gl_value_get_texture_target_mask (other_targets);

    if (GST_VALUE_HOLDS_LIST (format)) {
      gint j, len;

      len = gst_value_list_get_size (format);
      for (j = 0; j < len; j++) {
        const GValue *val;

        val = gst_value_list_get_value (format, j);
        if (G_VALUE_HOLDS_STRING (val)) {
          const gchar *format_str = g_value_get_string (val);
          GstVideoFormat v_format = gst_video_format_from_string (format_str);
          score_format_target (in_info, targets_mask, v_format,
              other_targets_mask, &min_loss, &out_info, &target);
          if (min_loss == 0)
            break;
        }
      }
    } else if (G_VALUE_HOLDS_STRING (format)) {
      const gchar *format_str = g_value_get_string (format);
      GstVideoFormat v_format = gst_video_format_from_string (format_str);
      score_format_target (in_info, targets_mask, v_format, other_targets_mask,
          &min_loss, &out_info, &target);
    }
  }
  if (out_info)
    gst_structure_set (outs, "format", G_TYPE_STRING,
        GST_VIDEO_FORMAT_INFO_NAME (out_info), NULL);
  if (target != GST_GL_TEXTURE_TARGET_NONE)
    gst_structure_set (outs, "texture-target", G_TYPE_STRING,
        gst_gl_texture_target_to_string (target), NULL);
}

/**
 * gst_gl_color_convert_fixate_caps:
 * @context: a #GstGLContext to use for transforming @caps
 * @direction: a #GstPadDirection
 * @caps: (transfer none): the #GstCaps of @direction
 * @other: (transfer full): the #GstCaps to fixate
 *
 * Provides an implementation of #GstBaseTransformClass.fixate_caps()
 *
 * Returns: (transfer full): the fixated #GstCaps
 *
 * Since: 1.8
 */
GstCaps *
gst_gl_color_convert_fixate_caps (GstGLContext * context,
    GstPadDirection direction, GstCaps * caps, GstCaps * other)
{
  GstCaps *result;

  result = gst_caps_intersect (other, caps);
  if (gst_caps_is_empty (result)) {
    gst_caps_unref (result);
    result = other;
  } else {
    gst_caps_unref (other);
  }

  result = gst_caps_make_writable (result);
  gst_gl_color_convert_fixate_format_target (caps, result);

  result = gst_caps_fixate (result);

  if (direction == GST_PAD_SINK) {
    if (gst_caps_is_subset (caps, result)) {
      gst_caps_replace (&result, caps);
    }
  }

  return result;
}

/**
 * gst_gl_color_convert_perform:
 * @convert: a #GstGLColorConvert
 * @inbuf: (transfer none): the #GstGLMemory filled #GstBuffer to convert
 *
 * Converts the data contained by @inbuf using the formats specified by the
 * #GstCaps passed to gst_gl_color_convert_set_caps()
 *
 * Returns: (transfer full) (nullable): a converted #GstBuffer or %NULL
 *
 * Since: 1.4
 */
GstBuffer *
gst_gl_color_convert_perform (GstGLColorConvert * convert, GstBuffer * inbuf)
{
  GstBuffer *ret;

  g_return_val_if_fail (convert != NULL, FALSE);

  GST_OBJECT_LOCK (convert);
  ret = _gst_gl_color_convert_perform_unlocked (convert, inbuf);
  GST_OBJECT_UNLOCK (convert);

  return ret;
}

static GstBuffer *
_gst_gl_color_convert_perform_unlocked (GstGLColorConvert * convert,
    GstBuffer * inbuf)
{
  g_return_val_if_fail (convert != NULL, FALSE);
  g_return_val_if_fail (inbuf, FALSE);

  if (G_UNLIKELY (convert->passthrough))
    return gst_buffer_ref (inbuf);

  convert->inbuf = inbuf;

  gst_gl_context_thread_add (convert->context,
      (GstGLContextThreadFunc) _do_convert, convert);

  if (!convert->priv->result) {
    if (convert->outbuf)
      gst_buffer_unref (convert->outbuf);
    convert->outbuf = NULL;
    return NULL;
  }

  return convert->outbuf;
}

static inline gboolean
_is_RGBx (GstVideoFormat v_format)
{
  switch (v_format) {
    case GST_VIDEO_FORMAT_RGBx:
    case GST_VIDEO_FORMAT_xRGB:
    case GST_VIDEO_FORMAT_BGRx:
    case GST_VIDEO_FORMAT_xBGR:
      return TRUE;
    default:
      return FALSE;
  }
}

static inline gboolean
_is_planar_rgb (GstVideoFormat v_format)
{
  switch (v_format) {
    case GST_VIDEO_FORMAT_GBR:
    case GST_VIDEO_FORMAT_RGBP:
    case GST_VIDEO_FORMAT_BGRP:
    case GST_VIDEO_FORMAT_GBR_10BE:
    case GST_VIDEO_FORMAT_GBR_10LE:
    case GST_VIDEO_FORMAT_GBRA:
    case GST_VIDEO_FORMAT_GBRA_10BE:
    case GST_VIDEO_FORMAT_GBRA_10LE:
    case GST_VIDEO_FORMAT_GBR_12BE:
    case GST_VIDEO_FORMAT_GBR_12LE:
    case GST_VIDEO_FORMAT_GBRA_12BE:
    case GST_VIDEO_FORMAT_GBRA_12LE:
      return TRUE;
    default:
      return FALSE;
  }
}

static void
video_format_to_gl_reorder (GstVideoFormat v_format, gint * reorder,
    gboolean input)
{
  switch (v_format) {
    case GST_VIDEO_FORMAT_v210:
      /* complex, handled in shader */
      reorder[0] = 0;
      reorder[1] = 1;
      reorder[2] = 2;
      reorder[3] = 3;
      break;
    case GST_VIDEO_FORMAT_UYVY:
      reorder[0] = 1;
      reorder[1] = 0;
      reorder[2] = input ? 0 : 2;
      reorder[3] = 0;
      break;
    case GST_VIDEO_FORMAT_YUY2:
    case GST_VIDEO_FORMAT_Y210:
    case GST_VIDEO_FORMAT_Y212_LE:
    case GST_VIDEO_FORMAT_Y212_BE:
      reorder[0] = 0;
      reorder[1] = 1;
      reorder[2] = input ? 1 : 0;
      reorder[3] = 2;
      break;
    case GST_VIDEO_FORMAT_GBR:
      if (input) {
        reorder[0] = 2;
        reorder[1] = 0;
        reorder[2] = 1;
      } else {
        reorder[0] = 0;
        reorder[1] = 1;
        reorder[2] = 2;
      }
      reorder[3] = 3;
      break;
    default:
      if (!gst_gl_video_format_swizzle (v_format, reorder))
        g_assert_not_reached ();
      break;
  }

  GST_TRACE ("swizzle: %u, %u, %u, %u", reorder[0], reorder[1], reorder[2],
      reorder[3]);
}

static void
calculate_reorder_indexes (GstVideoFormat in_format,
    GstVideoFormat out_format,
    int ret_in[GST_VIDEO_MAX_COMPONENTS], int ret_out[GST_VIDEO_MAX_COMPONENTS])
{
  int in_reorder[GST_VIDEO_MAX_COMPONENTS] = { 0, };
  int out_reorder[GST_VIDEO_MAX_COMPONENTS] = { 0, };
  int i;

  video_format_to_gl_reorder (in_format, in_reorder, TRUE);

  video_format_to_gl_reorder (out_format, out_reorder, FALSE);

  /* find the identity order for RGBA->$format */
  if (out_format == GST_VIDEO_FORMAT_YUY2
      || out_format == GST_VIDEO_FORMAT_UYVY) {
    for (i = 0; i < GST_VIDEO_MAX_COMPONENTS; i++)
      ret_out[i] = out_reorder[i];
  } else {
    gst_gl_swizzle_invert (out_reorder, ret_out);
  }

  for (i = 0; i < GST_VIDEO_MAX_COMPONENTS; i++)
    ret_in[i] = in_reorder[i];
  GST_TRACE ("in reorder: %u, %u, %u, %u", ret_in[0], ret_in[1], ret_in[2],
      ret_in[3]);
  GST_TRACE ("out reorder: %u, %u, %u, %u", ret_out[0], ret_out[1], ret_out[2],
      ret_out[3]);
}

static guint
_get_n_textures (GstVideoFormat v_format)
{
  const GstVideoFormatInfo *finfo = gst_video_format_get_info (v_format);

  return finfo->n_planes;
}

static gboolean
format_is_planar (GstVideoFormat v_format)
{
  const GstVideoFormatInfo *finfo = gst_video_format_get_info (v_format);

  return finfo->n_planes == finfo->n_components;
}

static void
_PLANAR_RGB_to_PLANAR_RGB (GstGLColorConvert * convert)
{
  struct ConvertInfo *info = &convert->priv->convert_info;
  GstVideoFormat in_format = GST_VIDEO_INFO_FORMAT (&convert->in_info);
  GstVideoFormat out_format = GST_VIDEO_INFO_FORMAT (&convert->out_info);
  const gchar *in_alpha = NULL;
  gchar *out_alpha = NULL;

  calculate_reorder_indexes (in_format, out_format, info->input_swizzle,
      info->output_swizzle);

  info->frag_prog = NULL;

  if (GST_VIDEO_INFO_HAS_ALPHA (&convert->in_info)) {
    in_alpha = "rgba.a = texture2D(Atex, texcoord * tex_scale3).r;";
    info->shader_tex_names[0] = "Rtex";
    info->shader_tex_names[1] = "Gtex";
    info->shader_tex_names[2] = "Btex";
    info->shader_tex_names[3] = "Atex";
  } else {
    in_alpha = "rgba.a = 1.0;";
    info->shader_tex_names[0] = "Rtex";
    info->shader_tex_names[1] = "Gtex";
    info->shader_tex_names[2] = "Btex";
  }

  if (GST_VIDEO_INFO_HAS_ALPHA (&convert->out_info)) {
    out_alpha = g_strdup ("gl_FragData[3] = vec4(rgba.a, 0, 0, 1.0);\n");
    info->out_n_textures = 4;
  } else {
    out_alpha = g_strdup ("\n");
    info->out_n_textures = 3;
  }

  info->templ = &templ_PLANAR_RGB_to_PLANAR_RGB;
  info->frag_body =
      g_strdup_printf (templ_PLANAR_RGB_to_PLANAR_RGB_BODY, in_alpha,
      out_alpha);

  g_free (out_alpha);
}

static void
_PLANAR_RGB_to_PACKED_RGB (GstGLColorConvert * convert)
{
  struct ConvertInfo *info = &convert->priv->convert_info;
  GstVideoFormat in_format = GST_VIDEO_INFO_FORMAT (&convert->in_info);
  GstVideoFormat out_format = GST_VIDEO_INFO_FORMAT (&convert->out_info);
  const gchar *alpha = NULL;

  info->frag_prog = NULL;

  calculate_reorder_indexes (in_format, out_format, info->input_swizzle,
      info->output_swizzle);

  if (GST_VIDEO_INFO_HAS_ALPHA (&convert->in_info)) {
    alpha = "rgba.a = texture2D(Atex, texcoord * tex_scale3).r;";
    info->shader_tex_names[0] = "Rtex";
    info->shader_tex_names[1] = "Gtex";
    info->shader_tex_names[2] = "Btex";
    info->shader_tex_names[3] = "Atex";
  } else {
    alpha = "rgba.a = 1.0;";
    info->shader_tex_names[0] = "Rtex";
    info->shader_tex_names[1] = "Gtex";
    info->shader_tex_names[2] = "Btex";
  }

  info->out_n_textures = 1;

  info->templ = &templ_PLANAR_RGB_to_PACKED_RGB;
  info->frag_body =
      g_strdup_printf (templ_PLANAR_RGB_to_PACKED_RGB_BODY, alpha);
}

static void
_PACKED_RGB_to_PLANAR_RGB (GstGLColorConvert * convert)
{
  struct ConvertInfo *info = &convert->priv->convert_info;
  GstVideoFormat in_format = GST_VIDEO_INFO_FORMAT (&convert->in_info);
  GstVideoFormat out_format = GST_VIDEO_INFO_FORMAT (&convert->out_info);
  const gchar *alpha;

  calculate_reorder_indexes (in_format, out_format, info->input_swizzle,
      info->output_swizzle);

  info->frag_prog = NULL;
  info->shader_tex_names[0] = "tex";

  if (GST_VIDEO_INFO_HAS_ALPHA (&convert->out_info)) {
    alpha = "gl_FragData[3] = vec4(rgba.a, 0, 0, 1.0);";
    info->out_n_textures = 4;
  } else {
    alpha = "";
    info->out_n_textures = 3;
  }

  info->templ = &templ_PACKED_RGB_to_PLANAR_RGB;
  info->frag_body =
      g_strdup_printf (templ_PACKED_RGB_to_PLANAR_RGB_BODY, alpha);
  info->shader_tex_names[0] = "tex";
}

static void
_PACKED_RGB_to_PACKED_RGB (GstGLColorConvert * convert)
{
  struct ConvertInfo *info = &convert->priv->convert_info;
  GstVideoFormat in_format = GST_VIDEO_INFO_FORMAT (&convert->in_info);
  GstVideoFormat out_format = GST_VIDEO_INFO_FORMAT (&convert->out_info);

  if (_is_RGBx (in_format)) {
    info->templ = &templ_REORDER_OVERWRITE_ALPHA;
    info->frag_body = g_strdup (templ_REORDER_OVERWRITE_ALPHA_BODY);
  } else {
    info->templ = &templ_REORDER;
    info->frag_body = g_strdup (templ_REORDER_BODY);
  }
  info->shader_tex_names[0] = "tex";
  calculate_reorder_indexes (in_format, out_format, info->input_swizzle,
      info->output_swizzle);
}

static void
_RGB_to_RGB (GstGLColorConvert * convert)
{
  GstVideoFormat in_format = GST_VIDEO_INFO_FORMAT (&convert->in_info);
  GstVideoFormat out_format = GST_VIDEO_INFO_FORMAT (&convert->out_info);

  if (_is_planar_rgb (in_format)) {
    if (_is_planar_rgb (out_format))
      _PLANAR_RGB_to_PLANAR_RGB (convert);
    else
      _PLANAR_RGB_to_PACKED_RGB (convert);
  } else {
    if (_is_planar_rgb (out_format))
      _PACKED_RGB_to_PLANAR_RGB (convert);
    else
      _PACKED_RGB_to_PACKED_RGB (convert);
  }
}

static void
_YUV_to_RGB (GstGLColorConvert * convert)
{
  struct ConvertInfo *info = &convert->priv->convert_info;
  GstVideoFormat in_format = GST_VIDEO_INFO_FORMAT (&convert->in_info);
  const GstVideoFormatInfo *in_finfo = gst_video_format_get_info (in_format);
  GstVideoFormat out_format = GST_VIDEO_INFO_FORMAT (&convert->out_info);
  gboolean apple_ycbcr = gst_gl_context_check_feature (convert->context,
      "GL_APPLE_ycbcr_422");
  gboolean in_tex_rectangular = FALSE;

#if GST_GL_HAVE_OPENGL
  GstMemory *memory = gst_buffer_peek_memory (convert->inbuf, 0);
  if (gst_is_gl_memory (memory) && (USING_OPENGL (convert->context)
          || USING_OPENGL3 (convert->context))) {
    in_tex_rectangular =
        convert->priv->from_texture_target == GST_GL_TEXTURE_TARGET_RECTANGLE;
  }
#endif

  info->out_n_textures = 1;
  info->out_bitdepth_factor = 1.0;

  calculate_reorder_indexes (in_format, out_format, info->input_swizzle,
      info->output_swizzle);

  if (in_tex_rectangular && apple_ycbcr
      && gst_buffer_n_memory (convert->inbuf) == 1) {
    /* FIXME: We should probably also check if tex_target actually is using
     * the Apple YCbCr422 extension. It could also be a normal UYVY texture
     * with RB or Lum/Alpha
     */
    /* The mangling will change this to the correct texture2DRect, sampler2DRect
     * for us */
    info->templ = &templ_REORDER;
    info->frag_body = g_strdup (templ_REORDER_BODY);
    info->shader_tex_names[0] = "tex";
  } else if (in_finfo->n_planes >= 3 && format_is_planar (in_format)) {
    info->shader_tex_names[0] = "Ytex";
    info->shader_tex_names[1] = "Utex";
    info->shader_tex_names[2] = "Vtex";
    info->in_bitdepth_factor =
        (float) ((1 << GST_ROUND_UP_8 (in_finfo->bits)) -
        1) / (float) ((1 << in_finfo->bits) - 1);
    if (GST_VIDEO_FORMAT_INFO_HAS_ALPHA (in_finfo)) {
      info->templ = &templ_A420_to_RGB;
      info->frag_body = g_strdup_printf (templ_PLANAR_YUVA_to_RGB_BODY);
      info->shader_tex_names[3] = "Atex";
    } else {
      info->templ = &templ_PLANAR_YUV_to_RGB;
      info->frag_body = g_strdup (templ_PLANAR_YUV_to_RGB_BODY);
    }
  } else {
    switch (in_format) {
      case GST_VIDEO_FORMAT_AYUV:
      case GST_VIDEO_FORMAT_VUYA:
      case GST_VIDEO_FORMAT_Y410:
      case GST_VIDEO_FORMAT_Y412_LE:
      case GST_VIDEO_FORMAT_Y412_BE:
        info->templ = &templ_AYUV_to_RGB;
        info->frag_body = g_strdup (templ_AYUV_to_RGB_BODY);
        info->shader_tex_names[0] = "tex";
        break;
      case GST_VIDEO_FORMAT_YUY2:
      {
        if (convert->priv->in_tex_formats[0] == GST_GL_LUMINANCE_ALPHA) {
          info->input_swizzle[1] = 3;
          info->input_swizzle[2] = 3;
        }
        info->templ = &templ_YUY2_UYVY_to_RGB;
        info->frag_body = g_strdup (templ_YUY2_UYVY_to_RGB_BODY);
        info->shader_tex_names[0] = "Ytex";
        break;
      }
      case GST_VIDEO_FORMAT_UYVY:
      {
        if (convert->priv->in_tex_formats[0] == GST_GL_LUMINANCE_ALPHA) {
          info->input_swizzle[0] = 3;
        }
        info->templ = &templ_YUY2_UYVY_to_RGB;
        info->frag_body = g_strdup (templ_YUY2_UYVY_to_RGB_BODY);
        info->shader_tex_names[0] = "Ytex";
        break;
      }
      case GST_VIDEO_FORMAT_Y210:
      case GST_VIDEO_FORMAT_Y212_LE:
      case GST_VIDEO_FORMAT_Y212_BE:
      {
        info->templ = &templ_YUY2_UYVY_to_RGB;
        info->frag_body = g_strdup (templ_YUY2_UYVY_to_RGB_BODY);
        info->shader_tex_names[0] = "Ytex";
        break;
      }
      case GST_VIDEO_FORMAT_NV12:
      case GST_VIDEO_FORMAT_NV16:
      case GST_VIDEO_FORMAT_NV21:
      case GST_VIDEO_FORMAT_NV61:
      case GST_VIDEO_FORMAT_P010_10LE:
      case GST_VIDEO_FORMAT_P010_10BE:
      case GST_VIDEO_FORMAT_P012_LE:
      case GST_VIDEO_FORMAT_P012_BE:
      case GST_VIDEO_FORMAT_P016_LE:
      case GST_VIDEO_FORMAT_P016_BE:
      {
        char val2 =
            convert->priv->in_tex_formats[1] ==
            GST_GL_LUMINANCE_ALPHA ? 'a' : 'g';
        info->templ = &templ_SEMI_PLANAR_to_RGB;
        info->frag_body =
            g_strdup_printf (templ_SEMI_PLANAR_to_RGB_BODY, val2,
            "yuva.a = 1.0;\n");
        info->shader_tex_names[0] = "Ytex";
        info->shader_tex_names[1] = "UVtex";
        break;
      }
      case GST_VIDEO_FORMAT_AV12:
      {
        char val2 =
            convert->priv->in_tex_formats[1] ==
            GST_GL_LUMINANCE_ALPHA ? 'a' : 'g';
        info->templ = &templ_AV12_to_RGB;
        info->frag_body =
            g_strdup_printf (templ_SEMI_PLANAR_to_RGB_BODY, val2,
            "yuva.a = texture2D(Atex, texcoord * tex_scale2).r;\n");
        info->shader_tex_names[0] = "Ytex";
        info->shader_tex_names[1] = "UVtex";
        info->shader_tex_names[2] = "Atex";
        break;
      }
      case GST_VIDEO_FORMAT_NV12_16L32S:
      {
        char val2 =
            convert->priv->in_tex_formats[1] ==
            GST_GL_LUMINANCE_ALPHA ? 'a' : 'g';
        info->templ = &templ_TILED_SEMI_PLANAR_to_RGB;
        info->frag_body = g_strdup_printf (templ_TILED_SEMI_PLANAR_to_RGB_BODY,
            16, 32, 8, 16, 'r', val2);
        info->shader_tex_names[0] = "Ytex";
        info->shader_tex_names[1] = "UVtex";
        break;
      }
      case GST_VIDEO_FORMAT_NV12_4L4:
      {
        char val2 =
            convert->priv->in_tex_formats[1] ==
            GST_GL_LUMINANCE_ALPHA ? 'a' : 'g';
        info->templ = &templ_TILED_SEMI_PLANAR_to_RGB;
        info->frag_body = g_strdup_printf (templ_TILED_SEMI_PLANAR_to_RGB_BODY,
            4, 4, 2, 4, 'r', val2);
        info->shader_tex_names[0] = "Ytex";
        info->shader_tex_names[1] = "UVtex";
        break;
      }
      case GST_VIDEO_FORMAT_v210:
      {
        info->templ = &templ_v210_to_RGB;
        info->frag_body = g_strdup (templ_v210_to_RGB_BODY);
        info->shader_tex_names[0] = "Ytex";
        break;
      }
      default:
        break;
    }
  }

  convert_to_RGB (info, &convert->in_info);
}

static void
_RGB_to_YUV (GstGLColorConvert * convert)
{
  struct ConvertInfo *info = &convert->priv->convert_info;
  GstVideoFormat in_format = GST_VIDEO_INFO_FORMAT (&convert->in_info);
  GstVideoFormat out_format = GST_VIDEO_INFO_FORMAT (&convert->out_info);
  const GstVideoFormatInfo *out_finfo = gst_video_format_get_info (out_format);
  const gchar *alpha;

  info->frag_prog = NULL;

  info->shader_tex_names[0] = "tex";

  calculate_reorder_indexes (in_format, out_format, info->input_swizzle,
      info->output_swizzle);
  if (out_finfo->n_planes >= 3 && format_is_planar (out_format)) {
    if (GST_VIDEO_FORMAT_INFO_HAS_ALPHA (out_finfo)) {
      info->templ = &templ_RGB_to_PLANAR_YUVA;
      info->frag_body = g_strdup (templ_RGB_to_PLANAR_YUVA_BODY);
    } else {
      info->templ = &templ_RGB_to_PLANAR_YUV;
      info->frag_body = g_strdup (templ_RGB_to_PLANAR_YUV_BODY);
    }
    info->chroma_sampling[0] = (float) (1 << out_finfo->w_sub[1]);
    info->chroma_sampling[1] = (float) (1 << out_finfo->h_sub[1]);
    info->out_bitdepth_factor =
        (float) ((1 << out_finfo->bits) -
        1) / (float) ((1 << GST_ROUND_UP_8 (out_finfo->bits)) - 1);
  } else {
    switch (out_format) {
      case GST_VIDEO_FORMAT_AYUV:
        alpha = _is_RGBx (in_format) ? "1.0" : "texel.a";
        info->templ = &templ_RGB_to_AYUV;
        info->frag_body = g_strdup_printf (templ_RGB_to_AYUV_BODY, alpha);
        break;
      case GST_VIDEO_FORMAT_VUYA:
        alpha = _is_RGBx (in_format) ? "1.0" : "texel.a";
        info->templ = &templ_RGB_to_AYUV;
        info->frag_body = g_strdup_printf (templ_RGB_to_AYUV_BODY, alpha);
        break;
      case GST_VIDEO_FORMAT_Y410:
      case GST_VIDEO_FORMAT_Y412_LE:
      case GST_VIDEO_FORMAT_Y412_BE:
        alpha = _is_RGBx (in_format) ? "1.0" : "texel.a";
        info->templ = &templ_RGB_to_AYUV;
        info->frag_body = g_strdup_printf (templ_RGB_to_AYUV_BODY, alpha);
        break;
      case GST_VIDEO_FORMAT_YUY2:
      case GST_VIDEO_FORMAT_Y210:
      case GST_VIDEO_FORMAT_Y212_LE:
      case GST_VIDEO_FORMAT_Y212_BE:
        info->templ = &templ_RGB_to_YUY2_UYVY;
        info->frag_body = g_strdup (templ_RGB_to_YUY2_UYVY_BODY);
        break;
      case GST_VIDEO_FORMAT_UYVY:
        info->templ = &templ_RGB_to_YUY2_UYVY;
        info->frag_body = g_strdup (templ_RGB_to_YUY2_UYVY_BODY);
        break;
      case GST_VIDEO_FORMAT_NV12:
      case GST_VIDEO_FORMAT_NV16:
        info->templ = &templ_RGB_to_SEMI_PLANAR_YUV;
        info->frag_body =
            g_strdup_printf (templ_RGB_to_SEMI_PLANAR_YUV_BODY, "");
        if (out_format == GST_VIDEO_FORMAT_NV16) {
          info->chroma_sampling[0] = 2.0f;
          info->chroma_sampling[1] = 1.0f;
        } else {
          info->chroma_sampling[0] = info->chroma_sampling[1] = 2.0f;
        }
        break;
      case GST_VIDEO_FORMAT_AV12:
        info->templ = &templ_RGB_to_SEMI_PLANAR_YUV;
        info->frag_body = g_strdup_printf (templ_RGB_to_SEMI_PLANAR_YUV_BODY,
            "gl_FragData[2] = vec4(yuva.a, 0.0, 0.0, 1.0);\n");
        info->chroma_sampling[0] = info->chroma_sampling[1] = 2.0f;
        break;
      case GST_VIDEO_FORMAT_NV21:
      case GST_VIDEO_FORMAT_NV61:
        info->templ = &templ_RGB_to_SEMI_PLANAR_YUV;
        info->frag_body =
            g_strdup_printf (templ_RGB_to_SEMI_PLANAR_YUV_BODY, "");
        if (out_format == GST_VIDEO_FORMAT_NV61) {
          info->chroma_sampling[0] = 2.0f;
          info->chroma_sampling[1] = 1.0f;
        } else {
          info->chroma_sampling[0] = info->chroma_sampling[1] = 2.0f;
        }
        break;
      case GST_VIDEO_FORMAT_v210:
        info->templ = &templ_RGB_to_v210;
        info->frag_body = g_strdup (templ_RGB_to_v210_BODY);
        break;
      default:
        break;
    }
  }

  convert_to_YUV (info, &convert->out_info);
}

static void
_YUV_to_YUV (GstGLColorConvert * convert)
{
  struct ConvertInfo *info = &convert->priv->convert_info;
  GstVideoFormat in_format = GST_VIDEO_INFO_FORMAT (&convert->in_info);
  const GstVideoFormatInfo *in_finfo = gst_video_format_get_info (in_format);
  GstVideoFormat out_format = GST_VIDEO_INFO_FORMAT (&convert->out_info);
  const GstVideoFormatInfo *out_finfo = gst_video_format_get_info (out_format);
  gboolean apple_ycbcr = gst_gl_context_check_feature (convert->context,
      "GL_APPLE_ycbcr_422");
  gboolean in_tex_rectangular = FALSE;
  gboolean input_planar = format_is_planar (in_format);
  gboolean output_planar = format_is_planar (out_format);

#if GST_GL_HAVE_OPENGL
  GstMemory *memory = gst_buffer_peek_memory (convert->inbuf, 0);
  if (gst_is_gl_memory (memory) && (USING_OPENGL (convert->context)
          || USING_OPENGL3 (convert->context))) {
    in_tex_rectangular =
        convert->priv->from_texture_target == GST_GL_TEXTURE_TARGET_RECTANGLE;
  }
#endif

  calculate_reorder_indexes (in_format, out_format, info->input_swizzle,
      info->output_swizzle);

  if (in_tex_rectangular && apple_ycbcr
      && gst_buffer_n_memory (convert->inbuf) == 1) {
    /* FIXME: We should probably also check if tex_target actually is using
     * the Apple YCbCr422 extension. It could also be a normal UYVY texture
     * with RB or Lum/Alpha
     */
    /* The mangling will change this to the correct texture2DRect, sampler2DRect
     * for us */
    info->templ = &templ_REORDER;
    info->frag_body = g_strdup (templ_REORDER_BODY);
    info->shader_tex_names[0] = "tex";
  } else if (input_planar && output_planar) {
    info->chroma_sampling[0] = (float) (1 << out_finfo->w_sub[1]);
    info->chroma_sampling[1] = (float) (1 << out_finfo->h_sub[1]);
    info->shader_tex_names[0] = "Ytex";
    info->shader_tex_names[1] = "Utex";
    info->shader_tex_names[2] = "Vtex";
    info->in_bitdepth_factor =
        (float) ((1 << GST_ROUND_UP_8 (in_finfo->bits)) -
        1) / (float) ((1 << in_finfo->bits) - 1);
    info->out_bitdepth_factor =
        (float) ((1 << out_finfo->bits) -
        1) / (float) ((1 << GST_ROUND_UP_8 (out_finfo->bits)) - 1);
    if (!GST_VIDEO_FORMAT_INFO_HAS_ALPHA (in_finfo)) {
      if (!GST_VIDEO_FORMAT_INFO_HAS_ALPHA (out_finfo)) {
        info->templ = &templ_PLANAR_YUV_to_PLANAR_YUV;
        info->frag_body = g_strdup (templ_PLANAR_YUV_to_PLANAR_YUV_BODY);
      } else {
        info->templ = &templ_PLANAR_YUV_to_PLANAR_YUVA;
        info->frag_body = g_strdup (templ_PLANAR_YUV_to_PLANAR_YUVA_BODY);
      }
    } else {
      info->shader_tex_names[3] = "Atex";
      if (!GST_VIDEO_FORMAT_INFO_HAS_ALPHA (out_finfo)) {
        info->templ = &templ_PLANAR_YUVA_to_PLANAR_YUV;
        info->frag_body = g_strdup (templ_PLANAR_YUVA_to_PLANAR_YUV_BODY);
      } else {
        info->templ = &templ_PLANAR_YUVA_to_PLANAR_YUVA;
        info->frag_body = g_strdup (templ_PLANAR_YUVA_to_PLANAR_YUVA_BODY);
      }
    }
  } else if (input_planar) {
    info->chroma_sampling[0] = (float) (1 << in_finfo->w_sub[1]);
    info->chroma_sampling[1] = (float) (1 << in_finfo->h_sub[1]);
    info->shader_tex_names[0] = "Ytex";
    info->shader_tex_names[1] = "Utex";
    info->shader_tex_names[2] = "Vtex";
    info->in_bitdepth_factor =
        (float) ((1 << GST_ROUND_UP_8 (in_finfo->bits)) -
        1) / (float) ((1 << in_finfo->bits) - 1);
    switch (out_format) {
      case GST_VIDEO_FORMAT_v210:
        info->templ = &templ_PLANAR_YUV_to_v210;
        info->frag_body = g_strdup (templ_PLANAR_YUV_to_v210_BODY);
        break;
      case GST_VIDEO_FORMAT_UYVY:
      case GST_VIDEO_FORMAT_YUY2:
        info->templ = &templ_PLANAR_YUV_to_YUY2_UYVY;
        info->frag_body = g_strdup (templ_PLANAR_YUV_to_YUY2_UYVY_BODY);
        break;
      default:
        g_assert_not_reached ();
        break;
    }
  } else if (output_planar) {
    info->chroma_sampling[0] = (float) (1 << out_finfo->w_sub[1]);
    info->chroma_sampling[1] = (float) (1 << out_finfo->h_sub[1]);
    info->out_bitdepth_factor =
        (float) ((1 << out_finfo->bits) -
        1) / (float) ((1 << GST_ROUND_UP_8 (out_finfo->bits)) - 1);
    switch (in_format) {
      case GST_VIDEO_FORMAT_v210:
        if (!GST_VIDEO_FORMAT_INFO_HAS_ALPHA (out_finfo)) {
          info->templ = &templ_v210_to_PLANAR_YUV;
          info->frag_body = g_strdup (templ_v210_to_PLANAR_YUV_BODY);
        } else {
          info->templ = &templ_v210_to_PLANAR_YUVA;
          info->frag_body = g_strdup (templ_v210_to_PLANAR_YUVA_BODY);
        }
        break;
      case GST_VIDEO_FORMAT_UYVY:
      case GST_VIDEO_FORMAT_YUY2:
        if (convert->priv->in_tex_formats[0] == GST_GL_LUMINANCE_ALPHA) {
          if (in_format == GST_VIDEO_FORMAT_UYVY) {
            info->input_swizzle[0] = 3;
          } else if (in_format == GST_VIDEO_FORMAT_YUY2) {
            info->input_swizzle[1] = 3;
            info->input_swizzle[2] = 3;
          }
        }
        if (!GST_VIDEO_FORMAT_INFO_HAS_ALPHA (out_finfo)) {
          info->templ = &templ_YUY2_UYVY_to_PLANAR_YUV;
          info->frag_body = g_strdup (templ_YUY2_UYVY_to_PLANAR_YUV_BODY);
        } else {
          info->templ = &templ_YUY2_UYVY_to_PLANAR_YUVA;
          info->frag_body = g_strdup (templ_YUY2_UYVY_to_PLANAR_YUVA_BODY);
        }
        break;
      default:
        g_assert_not_reached ();
        break;
    }
  } else {
    switch (in_format) {
      case GST_VIDEO_FORMAT_UYVY:
      case GST_VIDEO_FORMAT_YUY2:
        info->shader_tex_names[0] = "tex";
        switch (out_format) {
          case GST_VIDEO_FORMAT_v210:
            info->templ = &templ_YUY2_UYVY_to_v210;
            info->frag_body = g_strdup (templ_YUY2_UYVY_to_v210_BODY);
            break;
          case GST_VIDEO_FORMAT_YUY2:
          case GST_VIDEO_FORMAT_UYVY:
            info->templ = &templ_YUY2_UYVY_to_YUY2_UYVY;
            info->frag_body = g_strdup (templ_YUY2_UYVY_to_YUY2_UYVY_BODY);
            break;
          default:
            g_assert_not_reached ();
            break;
        }
        break;
      case GST_VIDEO_FORMAT_v210:
        info->shader_tex_names[0] = "tex";
        switch (out_format) {
          case GST_VIDEO_FORMAT_YUY2:
          case GST_VIDEO_FORMAT_UYVY:
            info->templ = &templ_v210_to_YUY2_UYVY;
            info->frag_body = g_strdup (templ_v210_to_YUY2_UYVY_BODY);
            break;
          default:
            g_assert_not_reached ();
            break;
        }
        break;
      default:
        g_assert_not_reached ();
        break;
    }
  }
}

static void
_RGB_to_GRAY (GstGLColorConvert * convert)
{
  struct ConvertInfo *info = &convert->priv->convert_info;
  GstVideoFormat in_format = GST_VIDEO_INFO_FORMAT (&convert->in_info);
  gchar *alpha = NULL;

  info->shader_tex_names[0] = "tex";

  if (_is_RGBx (in_format)) {
    info->templ = &templ_REORDER_OVERWRITE_ALPHA;
    info->frag_body = g_strdup (templ_REORDER_OVERWRITE_ALPHA_BODY);
  } else {
    info->templ = &templ_REORDER;
    info->frag_body = g_strdup_printf (templ_REORDER_BODY);
  }

  switch (GST_VIDEO_INFO_FORMAT (&convert->out_info)) {
    case GST_VIDEO_FORMAT_GRAY8:
      /* FIXME: currently broken */
      calculate_reorder_indexes (in_format, GST_VIDEO_FORMAT_RGBA,
          info->input_swizzle, info->output_swizzle);
      info->output_swizzle[0] = 0;
      info->output_swizzle[1] = 0;
      info->output_swizzle[2] = 0;
      info->output_swizzle[3] = 0;
      break;
    default:
      break;
  }

  g_free (alpha);
}

static void
_GRAY_to_RGB (GstGLColorConvert * convert)
{
  struct ConvertInfo *info = &convert->priv->convert_info;
  GstVideoFormat out_format = GST_VIDEO_INFO_FORMAT (&convert->out_info);

  info->shader_tex_names[0] = "tex";

  switch (GST_VIDEO_INFO_FORMAT (&convert->in_info)) {
    case GST_VIDEO_FORMAT_GRAY8:
      info->templ = &templ_REORDER;
      calculate_reorder_indexes (GST_VIDEO_FORMAT_RGBA, out_format,
          info->input_swizzle, info->output_swizzle);
      info->input_swizzle[0] = 0;
      info->input_swizzle[1] = 0;
      info->input_swizzle[2] = 0;
      info->input_swizzle[3] = 3;
      info->frag_body = g_strdup (templ_REORDER_BODY);
      break;
    case GST_VIDEO_FORMAT_GRAY16_LE:
    {
      calculate_reorder_indexes (GST_VIDEO_FORMAT_RGBA, out_format,
          info->input_swizzle, info->output_swizzle);
      info->templ = &templ_COMPOSE;
      info->input_swizzle[0] =
          convert->priv->in_tex_formats[0] == GST_GL_LUMINANCE_ALPHA ? 3 : 1;
      info->input_swizzle[1] = 0;
      info->frag_body = g_strdup (templ_COMPOSE_BODY);
      break;
    }
    case GST_VIDEO_FORMAT_GRAY16_BE:
    {
      calculate_reorder_indexes (GST_VIDEO_FORMAT_RGBA, out_format,
          info->input_swizzle, info->output_swizzle);
      info->templ = &templ_COMPOSE;
      info->input_swizzle[0] = 0;
      info->input_swizzle[1] =
          convert->priv->in_tex_formats[0] == GST_GL_LUMINANCE_ALPHA ? 3 : 1;
      info->frag_body = g_strdup (templ_COMPOSE_BODY);
      break;
    }
    default:
      break;
  }
}

static void
_bind_buffer (GstGLColorConvert * convert)
{
  const GstGLFuncs *gl = convert->context->gl_vtable;

  gl->BindBuffer (GL_ELEMENT_ARRAY_BUFFER, convert->priv->vbo_indices);
  gl->BindBuffer (GL_ARRAY_BUFFER, convert->priv->vertex_buffer);

  /* Load the vertex position */
  gl->VertexAttribPointer (convert->priv->attr_position, 3, GL_FLOAT, GL_FALSE,
      5 * sizeof (GLfloat), (void *) 0);
  gl->EnableVertexAttribArray (convert->priv->attr_position);

  if (convert->priv->attr_texture != -1) {
    /* Load the texture coordinate */
    gl->VertexAttribPointer (convert->priv->attr_texture, 2, GL_FLOAT, GL_FALSE,
        5 * sizeof (GLfloat), (void *) (3 * sizeof (GLfloat)));
    gl->EnableVertexAttribArray (convert->priv->attr_texture);
  }
}

static void
_unbind_buffer (GstGLColorConvert * convert)
{
  const GstGLFuncs *gl = convert->context->gl_vtable;

  gl->BindBuffer (GL_ELEMENT_ARRAY_BUFFER, 0);
  gl->DisableVertexAttribArray (convert->priv->attr_position);

  if (convert->priv->attr_texture != -1) {
    gl->BindBuffer (GL_ARRAY_BUFFER, 0);
    gl->DisableVertexAttribArray (convert->priv->attr_texture);
  }
}

static GstGLShader *
_create_shader (GstGLColorConvert * convert)
{
  struct ConvertInfo *info = &convert->priv->convert_info;
  GString *str = g_string_new (NULL);
  GstGLShader *ret = NULL;
  GstGLSLStage *stage;
  GstGLSLVersion version;
  GstGLSLProfile profile;
  gchar *version_str, *tmp, *tmp1;
  const gchar *strings[3];
  GError *error = NULL;
  int i;

  ret = gst_gl_shader_new (convert->context);

  tmp =
      _gst_glsl_mangle_shader (text_vertex_shader, GL_VERTEX_SHADER,
      info->templ->target, convert->priv->from_texture_target, convert->context,
      &version, &profile);

  tmp1 = gst_glsl_version_profile_to_string (version, profile);
  version_str = g_strdup_printf ("#version %s\n", tmp1);
  g_free (tmp1);

  strings[0] = version_str;
  strings[1] = tmp;
  if (!(stage = gst_glsl_stage_new_with_strings (convert->context,
              GL_VERTEX_SHADER, version, profile, 2, strings))) {
    GST_ERROR_OBJECT (convert, "Failed to create vertex stage");
    g_free (version_str);
    g_free (tmp);
    gst_object_unref (ret);
    return NULL;
  }
  g_free (tmp);

  if (!gst_gl_shader_compile_attach_stage (ret, stage, &error)) {
    GST_ERROR_OBJECT (convert, "Failed to compile vertex shader %s",
        error->message);
    g_clear_error (&error);
    g_free (version_str);
    gst_object_unref (stage);
    gst_object_unref (ret);
    return NULL;
  }

  if (info->templ->extensions)
    g_string_append (str, info->templ->extensions);

  if (convert->priv->from_texture_target == GST_GL_TEXTURE_TARGET_EXTERNAL_OES
      && info->templ->target != GST_GL_TEXTURE_TARGET_EXTERNAL_OES)
    g_string_append (str, glsl_OES_extension_string);

  g_string_append (str,
      gst_gl_shader_string_get_highest_precision (convert->context, version,
          profile));

  if (info->templ->uniforms)
    g_string_append (str, info->templ->uniforms);

  g_string_append_c (str, '\n');

  /* GL 3.3+ and GL ES 3.x */
  if ((profile == GST_GLSL_PROFILE_CORE && version >= GST_GLSL_VERSION_330)
      || (profile == GST_GLSL_PROFILE_ES && version >= GST_GLSL_VERSION_300)) {
    if (info->out_n_textures > 1) {
      gint i;

      for (i = 0; i < info->out_n_textures; i++) {
        g_string_append_printf (str,
            "layout(location = %d) out vec4 fragColor_%d;\n", i, i);
      }
    } else {
      g_string_append (str, "layout (location = 0) out vec4 fragColor;\n");
    }
  } else if (profile == GST_GLSL_PROFILE_CORE
      && version >= GST_GLSL_VERSION_150) {
    /* no layout specifiers, use glBindFragDataLocation instead */
    if (info->out_n_textures > 1) {
      gint i;

      for (i = 0; i < info->out_n_textures; i++) {
        gchar *var_name = g_strdup_printf ("fragColor_%d", i);
        g_string_append_printf (str, "out vec4 %s;\n", var_name);
        gst_gl_shader_bind_frag_data_location (ret, i, var_name);
        g_free (var_name);
      }
    } else {
      g_string_append (str, "out vec4 fragColor;\n");
      gst_gl_shader_bind_frag_data_location (ret, 0, "fragColor");
    }
  }

  for (i = 0; i < MAX_FUNCTIONS; i++) {
    if (info->templ->functions[i] == NULL)
      break;

    g_string_append_c (str, '\n');
    g_string_append (str, info->templ->functions[i]);
    g_string_append_c (str, '\n');
  }

  {
    const gchar *varying = NULL;

    if ((profile == GST_GLSL_PROFILE_ES && version >= GST_GLSL_VERSION_300)
        || (profile == GST_GLSL_PROFILE_CORE
            && version >= GST_GLSL_VERSION_150)) {
      varying = "in";
    } else {
      varying = "varying";
    }
    g_string_append_printf (str, "\n%s vec2 v_texcoord;\nvoid main (void) {\n",
        varying);
  }
  if (info->frag_body) {
    g_string_append (str, "vec2 texcoord;\n");
    if (convert->priv->from_texture_target == GST_GL_TEXTURE_TARGET_RECTANGLE
        && info->templ->target != GST_GL_TEXTURE_TARGET_RECTANGLE) {
      g_string_append (str, "texcoord = v_texcoord * vec2 (width, height);\n");
    } else {
      g_string_append (str, "texcoord = v_texcoord;\n");
    }

    g_string_append (str, info->frag_body);
  }
  g_string_append (str, "\n}");
  tmp = g_string_free (str, FALSE);
  info->frag_prog = _gst_glsl_mangle_shader (tmp, GL_FRAGMENT_SHADER,
      info->templ->target, convert->priv->from_texture_target, convert->context,
      &version, &profile);
  g_free (tmp);

  strings[1] = info->frag_prog;
  if (!(stage = gst_glsl_stage_new_with_strings (convert->context,
              GL_FRAGMENT_SHADER, version, profile, 2, strings))) {
    GST_ERROR_OBJECT (convert, "Failed to create fragment stage");
    g_free (info->frag_prog);
    info->frag_prog = NULL;
    g_free (version_str);
    gst_object_unref (ret);
    return NULL;
  }
  g_free (version_str);
  if (!gst_gl_shader_compile_attach_stage (ret, stage, &error)) {
    GST_ERROR_OBJECT (convert, "Failed to compile fragment shader %s",
        error->message);
    g_clear_error (&error);
    g_free (info->frag_prog);
    info->frag_prog = NULL;
    gst_object_unref (stage);
    gst_object_unref (ret);
    return NULL;
  }

  if (!gst_gl_shader_link (ret, &error)) {
    GST_ERROR_OBJECT (convert, "Failed to link shader %s", error->message);
    g_clear_error (&error);
    g_free (info->frag_prog);
    info->frag_prog = NULL;
    gst_object_unref (ret);
    return NULL;
  }

  return ret;
}

/* Called in the gl thread */
static gboolean
_init_convert (GstGLColorConvert * convert)
{
  GstGLFuncs *gl;
  struct ConvertInfo *info = &convert->priv->convert_info;
  gsize input_data_width = GST_VIDEO_INFO_WIDTH (&convert->in_info);
  gsize output_data_width = GST_VIDEO_INFO_WIDTH (&convert->out_info);
  gint i;

  gl = convert->context->gl_vtable;

  if (convert->initted)
    return TRUE;

  GST_INFO ("Initializing color conversion from %s to %s",
      gst_video_format_to_string (GST_VIDEO_INFO_FORMAT (&convert->in_info)),
      gst_video_format_to_string (GST_VIDEO_INFO_FORMAT (&convert->out_info)));

  if (!gl->CreateProgramObject && !gl->CreateProgram) {
    GST_ERROR_OBJECT (convert, "Cannot perform color conversion without "
        "OpenGL shaders");
    goto error;
  }

  memset (info, 0, sizeof (*info));
  info->in_n_textures =
      _get_n_textures (GST_VIDEO_INFO_FORMAT (&convert->in_info));
  info->out_n_textures =
      _get_n_textures (GST_VIDEO_INFO_FORMAT (&convert->out_info));

  matrix_set_identity (&info->to_RGB_matrix);
  matrix_set_identity (&info->to_YUV_matrix);

  if (GST_VIDEO_INFO_IS_RGB (&convert->in_info)) {
    if (GST_VIDEO_INFO_IS_RGB (&convert->out_info)) {
      _RGB_to_RGB (convert);
    }
  }

  if (GST_VIDEO_INFO_IS_YUV (&convert->in_info)) {
    if (GST_VIDEO_INFO_IS_RGB (&convert->out_info)) {
      _YUV_to_RGB (convert);
    }
  }

  if (GST_VIDEO_INFO_IS_RGB (&convert->in_info)) {
    if (GST_VIDEO_INFO_IS_YUV (&convert->out_info)) {
      _RGB_to_YUV (convert);
    }
  }

  if (GST_VIDEO_INFO_IS_YUV (&convert->in_info)) {
    if (GST_VIDEO_INFO_IS_YUV (&convert->out_info)) {
      _YUV_to_YUV (convert);
    }
  }

  if (GST_VIDEO_INFO_IS_RGB (&convert->in_info)) {
    if (GST_VIDEO_INFO_IS_GRAY (&convert->out_info)) {
      _RGB_to_GRAY (convert);
    }
  }

  if (GST_VIDEO_INFO_IS_GRAY (&convert->in_info)) {
    if (GST_VIDEO_INFO_IS_RGB (&convert->out_info)) {
      _GRAY_to_RGB (convert);
    }
  }

  if (!info->frag_body)
    goto unhandled_format;

  /* multiple draw targets not supported on GLES2... */
  if (info->out_n_textures > 1 && !gl->DrawBuffers) {
    GST_ERROR ("Conversion requires output to multiple draw buffers");
    goto incompatible_api;
  }

  /* Requires reading from a RG/LA framebuffer... */
  if (USING_GLES2 (convert->context) && !USING_GLES3 (convert->context) &&
      (GST_VIDEO_INFO_FORMAT (&convert->out_info) == GST_VIDEO_FORMAT_YUY2 ||
          GST_VIDEO_INFO_FORMAT (&convert->out_info) ==
          GST_VIDEO_FORMAT_UYVY)) {
    GST_ERROR ("Conversion requires reading with an unsupported format");
    goto incompatible_api;
  }

  /* Requires texelFetch() function... */
  if (!(USING_GLES3 (convert->context) || USING_OPENGL30 (convert->context)) &&
      GST_VIDEO_FORMAT_INFO_IS_TILED (convert->in_info.finfo)) {
    GST_ERROR ("Conversion requires texelFetch() function available since "
        "GLSL 1.30");
    goto incompatible_api;
  }

  if (!(convert->shader = _create_shader (convert)))
    goto error;

  convert->priv->attr_position =
      gst_gl_shader_get_attribute_location (convert->shader, "a_position");

  if (!GST_VIDEO_FORMAT_INFO_IS_TILED (convert->in_info.finfo))
    convert->priv->attr_texture =
        gst_gl_shader_get_attribute_location (convert->shader, "a_texcoord");
  else
    convert->priv->attr_texture = -1;

  gst_gl_shader_use (convert->shader);

  {
    gfloat m[16];
    matrix_to_float (&info->to_RGB_matrix, m);
    gst_gl_shader_set_uniform_matrix_4fv (convert->shader, "to_RGB_matrix", 1,
        FALSE, m);
    matrix_to_float (&info->to_YUV_matrix, m);
    gst_gl_shader_set_uniform_matrix_4fv (convert->shader, "to_YUV_matrix", 1,
        FALSE, m);
  }

  for (i = info->in_n_textures - 1; i >= 0; i--) {
    if (info->shader_tex_names[i])
      gst_gl_shader_set_uniform_1i (convert->shader, info->shader_tex_names[i],
          i);
  }

  if (GST_VIDEO_INFO_FORMAT (&convert->in_info) == GST_VIDEO_FORMAT_v210) {
    /* XXX: this may not work with strides larger than the minimum required, */
    input_data_width = ((GST_VIDEO_INFO_WIDTH (&convert->in_info) + 5) / 6) * 4;
  }

  if (GST_VIDEO_INFO_FORMAT (&convert->out_info) == GST_VIDEO_FORMAT_v210) {
    /* XXX: this may not work with strides larger than the minimum required, */
    output_data_width =
        ((GST_VIDEO_INFO_WIDTH (&convert->out_info) + 5) / 6) * 4;
  }

  if (GST_VIDEO_FORMAT_INFO_IS_TILED (convert->in_info.finfo)) {
    guint tile_width, tile_height;
    gsize stride;
    gfloat width, height;

    stride = GST_VIDEO_INFO_PLANE_STRIDE (&convert->in_info, 0);
    tile_width = GST_VIDEO_FORMAT_INFO_TILE_WIDTH (convert->in_info.finfo, 0);
    tile_height = GST_VIDEO_FORMAT_INFO_TILE_HEIGHT (convert->in_info.finfo, 0);

    width = GST_VIDEO_TILE_X_TILES (stride) * tile_width;
    height = GST_VIDEO_TILE_Y_TILES (stride) * tile_height;

    gst_gl_shader_set_uniform_1f (convert->shader, "width", width);
    gst_gl_shader_set_uniform_1f (convert->shader, "height", height);
  } else {
    gst_gl_shader_set_uniform_1f (convert->shader, "width", input_data_width);
    gst_gl_shader_set_uniform_1f (convert->shader, "height",
        GST_VIDEO_INFO_HEIGHT (&convert->in_info));
  }
  gst_gl_shader_set_uniform_1f (convert->shader, "out_width",
      output_data_width);
  gst_gl_shader_set_uniform_1f (convert->shader, "out_height",
      GST_VIDEO_INFO_HEIGHT (&convert->out_info));

  if (convert->priv->from_texture_target == GST_GL_TEXTURE_TARGET_RECTANGLE) {
    gst_gl_shader_set_uniform_1f (convert->shader, "poffset_x", 1.);
    gst_gl_shader_set_uniform_1f (convert->shader, "poffset_y", 1.);
    gst_gl_shader_set_uniform_2f (convert->shader, "vert_to_tex",
        (gfloat) input_data_width,
        (gfloat) GST_VIDEO_INFO_HEIGHT (&convert->in_info));
  } else {
    gst_gl_shader_set_uniform_1f (convert->shader, "poffset_x",
        1. / (gfloat) input_data_width);
    gst_gl_shader_set_uniform_1f (convert->shader, "poffset_y",
        1. / (gfloat) GST_VIDEO_INFO_HEIGHT (&convert->in_info));
    gst_gl_shader_set_uniform_2f (convert->shader, "vert_to_tex", 1., 1.);
  }

  if (info->chroma_sampling[0] > 0.0f && info->chroma_sampling[1] > 0.0f) {
    gst_gl_shader_set_uniform_2fv (convert->shader, "chroma_sampling", 1,
        info->chroma_sampling);
  }

  gst_gl_shader_set_uniform_1iv (convert->shader, "input_swizzle", 4,
      info->input_swizzle);
  gst_gl_shader_set_uniform_1iv (convert->shader, "output_swizzle", 4,
      info->output_swizzle);

  /* if we need to convert from a 10/12/14-in-16 bit format to the relevant
   * 0.0->1.0 float (or vice versa) */
  gst_gl_shader_set_uniform_1f (convert->shader, "in_bitdepth_factor",
      info->in_bitdepth_factor);
  gst_gl_shader_set_uniform_1f (convert->shader, "out_bitdepth_factor",
      info->out_bitdepth_factor);

  gst_gl_context_clear_shader (convert->context);

  if (convert->fbo == NULL && !_init_convert_fbo (convert)) {
    goto error;
  }

  if (!convert->priv->vertex_buffer) {
    if (gl->GenVertexArrays) {
      gl->GenVertexArrays (1, &convert->priv->vao);
      gl->BindVertexArray (convert->priv->vao);
    }

    gl->GenBuffers (1, &convert->priv->vertex_buffer);
    gl->BindBuffer (GL_ARRAY_BUFFER, convert->priv->vertex_buffer);
    gl->BufferData (GL_ARRAY_BUFFER, 4 * 5 * sizeof (GLfloat), vertices,
        GL_STATIC_DRAW);

    gl->GenBuffers (1, &convert->priv->vbo_indices);
    gl->BindBuffer (GL_ELEMENT_ARRAY_BUFFER, convert->priv->vbo_indices);
    gl->BufferData (GL_ELEMENT_ARRAY_BUFFER, sizeof (indices), indices,
        GL_STATIC_DRAW);

    if (gl->GenVertexArrays) {
      _bind_buffer (convert);
      gl->BindVertexArray (0);
    }

    gl->BindBuffer (GL_ARRAY_BUFFER, 0);
    gl->BindBuffer (GL_ELEMENT_ARRAY_BUFFER, 0);
  }

  gl->BindTexture (GL_TEXTURE_2D, 0);

  convert->initted = TRUE;

  return TRUE;

unhandled_format:
  GST_ERROR_OBJECT (convert, "Don't know how to convert from %s to %s",
      gst_video_format_to_string (GST_VIDEO_INFO_FORMAT (&convert->in_info)),
      gst_video_format_to_string (GST_VIDEO_INFO_FORMAT (&convert->out_info)));

error:
  return FALSE;

incompatible_api:
  {
    GST_ERROR_OBJECT (convert, "Converting from %s to %s requires "
        "functionality that the current OpenGL setup does not support",
        gst_video_format_to_string (GST_VIDEO_INFO_FORMAT (&convert->in_info)),
        gst_video_format_to_string (GST_VIDEO_INFO_FORMAT
            (&convert->out_info)));
    return FALSE;
  }
}

/* called by _init_convert (in the gl thread) */
static gboolean
_init_convert_fbo (GstGLColorConvert * convert)
{
  guint out_width, out_height;

  out_width = GST_VIDEO_INFO_WIDTH (&convert->out_info);
  out_height = GST_VIDEO_INFO_HEIGHT (&convert->out_info);

  convert->fbo =
      gst_gl_framebuffer_new_with_default_depth (convert->context, out_width,
      out_height);

  return convert->fbo != NULL;
}

static gboolean
_do_convert_one_view (GstGLContext * context, GstGLColorConvert * convert,
    guint view_num)
{
  guint in_width, in_height, out_width, out_height;
  struct ConvertInfo *c_info = &convert->priv->convert_info;
  GstMapInfo out_info[GST_VIDEO_MAX_PLANES], in_info[GST_VIDEO_MAX_PLANES];
  gboolean res = TRUE;
  gint i, j = 0;
  const gint in_plane_offset = view_num * c_info->in_n_textures;
  const gint out_plane_offset = view_num * c_info->out_n_textures;

  out_width = GST_VIDEO_INFO_WIDTH (&convert->out_info);
  out_height = GST_VIDEO_INFO_HEIGHT (&convert->out_info);
  in_width = GST_VIDEO_INFO_WIDTH (&convert->in_info);
  in_height = GST_VIDEO_INFO_HEIGHT (&convert->in_info);

  for (i = 0; i < c_info->in_n_textures; i++) {
    convert->priv->in_tex[i] =
        (GstGLMemory *) gst_buffer_peek_memory (convert->inbuf,
        i + in_plane_offset);
    if (!gst_is_gl_memory ((GstMemory *) convert->priv->in_tex[i])) {
      GST_ERROR_OBJECT (convert, "input must be GstGLMemory");
      res = FALSE;
      goto out;
    }
    if (convert->context != convert->priv->in_tex[i]->mem.context) {
      GST_ERROR_OBJECT (convert, "input memory OpenGL context is different. "
          "we have %" GST_PTR_FORMAT " memory has %" GST_PTR_FORMAT,
          convert->context, convert->priv->in_tex[i]->mem.context);
      res = FALSE;
      goto out;
    }

    if (!gst_memory_map ((GstMemory *) convert->priv->in_tex[i], &in_info[i],
            GST_MAP_READ | GST_MAP_GL)) {
      GST_ERROR_OBJECT (convert, "failed to map input memory %p",
          convert->priv->in_tex[i]);
      res = FALSE;
      goto out;
    }
  }

  for (j = 0; j < c_info->out_n_textures; j++) {
    GstGLMemory *out_tex =
        (GstGLMemory *) gst_buffer_peek_memory (convert->outbuf,
        j + out_plane_offset);
    gint mem_width, mem_height;

    if (!gst_is_gl_memory ((GstMemory *) out_tex)) {
      GST_ERROR_OBJECT (convert, "output must be GstGLMemory");
      res = FALSE;
      goto out;
    }
    if (convert->context != out_tex->mem.context) {
      GST_ERROR_OBJECT (convert, "output memory OpenGL context is different. "
          "we have %" GST_PTR_FORMAT " memory has %" GST_PTR_FORMAT,
          convert->context, out_tex->mem.context);
      res = FALSE;
      goto out;
    }

    mem_width = gst_gl_memory_get_texture_width (out_tex);
    mem_height = gst_gl_memory_get_texture_height (out_tex);

    if (out_tex->tex_format == GST_GL_LUMINANCE
        || out_tex->tex_format == GST_GL_LUMINANCE_ALPHA
        || out_height != mem_height
        || (out_width != mem_width
            && convert->out_info.finfo->format != GST_VIDEO_FORMAT_v210)) {
      /* Luminance formats are not color renderable */
      /* rendering to a framebuffer only renders the intersection of all
       * the attachments i.e. the smallest attachment size */
      if (!convert->priv->out_tex[j]) {
        GstGLVideoAllocationParams *params;
        GstGLBaseMemoryAllocator *base_mem_allocator;
        GstAllocator *allocator;
        GstVideoFormat temp_format = GST_VIDEO_FORMAT_RGBA;
        GstVideoInfo temp_info;
        GstGLFormat tex_format;

        if (convert->out_info.finfo->bits > 8) {
#if G_BYTE_ORDER == G_LITTLE_ENDIAN
          temp_format = GST_VIDEO_FORMAT_RGBA64_LE;
#else
          temp_format = GST_VIDEO_FORMAT_RGBA64_BE;
#endif
        }
        gst_video_info_set_format (&temp_info, temp_format, out_width,
            out_height);
        tex_format = gst_gl_format_from_video_info (context, &temp_info, 0);

        allocator = gst_allocator_find (GST_GL_MEMORY_ALLOCATOR_NAME);
        base_mem_allocator = GST_GL_BASE_MEMORY_ALLOCATOR (allocator);
        params = gst_gl_video_allocation_params_new (context, NULL, &temp_info,
            0, NULL, convert->priv->to_texture_target, tex_format);

        convert->priv->out_tex[j] =
            (GstGLMemory *) gst_gl_base_memory_alloc (base_mem_allocator,
            (GstGLAllocationParams *) params);

        gst_gl_allocation_params_free ((GstGLAllocationParams *) params);
        gst_object_unref (allocator);
      }
    } else {
      convert->priv->out_tex[j] = out_tex;
    }

    if (!gst_memory_map ((GstMemory *) convert->priv->out_tex[j], &out_info[j],
            GST_MAP_WRITE | GST_MAP_GL)) {
      GST_ERROR_OBJECT (convert, "failed to map output memory %p",
          convert->priv->out_tex[i]);
      res = FALSE;
      goto out;
    }
  }

  GST_LOG_OBJECT (convert, "converting to textures:%p,%p,%p,%p "
      "dimensions:%ux%u, from textures:%p,%p,%p,%p dimensions:%ux%u",
      convert->priv->out_tex[0], convert->priv->out_tex[1],
      convert->priv->out_tex[2], convert->priv->out_tex[3], out_width,
      out_height, convert->priv->in_tex[0], convert->priv->in_tex[1],
      convert->priv->in_tex[2], convert->priv->in_tex[3], in_width, in_height);

  if (!_do_convert_draw (context, convert))
    res = FALSE;

out:
  for (j--; j >= 0; j--) {
    GstGLMemory *out_tex =
        (GstGLMemory *) gst_buffer_peek_memory (convert->outbuf,
        j + out_plane_offset);
    gint mem_width, mem_height;

    gst_memory_unmap ((GstMemory *) convert->priv->out_tex[j], &out_info[j]);

    mem_width = gst_gl_memory_get_texture_width (out_tex);
    mem_height = gst_gl_memory_get_texture_height (out_tex);

    if (out_tex->tex_format == GST_GL_LUMINANCE
        || out_tex->tex_format == GST_GL_LUMINANCE_ALPHA
        || out_height != mem_height
        || (out_width != mem_width
            && convert->out_info.finfo->format != GST_VIDEO_FORMAT_v210)) {
      GstMapInfo to_info, from_info;

      if (!gst_memory_map ((GstMemory *) convert->priv->out_tex[j], &from_info,
              GST_MAP_READ | GST_MAP_GL)) {
        GST_ERROR_OBJECT (convert, "Failed to map intermediate memory");
        res = FALSE;
        continue;
      }
      if (!gst_memory_map ((GstMemory *) out_tex, &to_info,
              GST_MAP_WRITE | GST_MAP_GL)) {
        GST_ERROR_OBJECT (convert, "Failed to map intermediate memory");
        res = FALSE;
        continue;
      }
      gst_gl_memory_copy_into (convert->priv->out_tex[j],
          out_tex->tex_id, convert->priv->to_texture_target,
          out_tex->tex_format, mem_width, mem_height);
      gst_memory_unmap ((GstMemory *) convert->priv->out_tex[j], &from_info);
      gst_memory_unmap ((GstMemory *) out_tex, &to_info);
    } else {
      convert->priv->out_tex[j] = NULL;
    }
  }

  for (i--; i >= 0; i--) {
    gst_memory_unmap ((GstMemory *) convert->priv->in_tex[i], &in_info[i]);
  }

  return res;
}

/* Called by the idle function in the gl thread */
void
_do_convert (GstGLContext * context, GstGLColorConvert * convert)
{
  GstVideoInfo *in_info = &convert->in_info;
  struct ConvertInfo *c_info = &convert->priv->convert_info;
  gboolean res = TRUE;
  gint views, v;
  GstGLSyncMeta *sync_meta;
  GstFlowReturn ret;

  convert->outbuf = NULL;

  if (GST_VIDEO_INFO_MULTIVIEW_MODE (in_info) ==
      GST_VIDEO_MULTIVIEW_MODE_SEPARATED)
    views = GST_VIDEO_INFO_VIEWS (in_info);
  else
    views = 1;

  c_info->in_n_textures =
      _get_n_textures (GST_VIDEO_INFO_FORMAT (&convert->in_info));
  c_info->out_n_textures =
      _get_n_textures (GST_VIDEO_INFO_FORMAT (&convert->out_info));

  {
    gboolean tex_format_change = FALSE;
    guint i, v;

    for (v = 0; v < views; v++) {
      for (i = 0; i < c_info->in_n_textures; i++) {
        guint j = v * c_info->in_n_textures + i;
        GstGLMemory *in_tex =
            (GstGLMemory *) gst_buffer_peek_memory (convert->inbuf, j);
        if (!gst_is_gl_memory ((GstMemory *) in_tex)) {
          GST_ERROR_OBJECT (convert, "input must be GstGLMemory");
          convert->priv->result = FALSE;
          return;
        }

        if (j >= GST_VIDEO_MAX_PLANES)
          /* our arrays aren't that big */
          g_assert_not_reached ();

        if (v > 0 && in_tex->tex_format != convert->priv->in_tex_formats[i]) {
          GST_ERROR_OBJECT (convert, "Cannot convert textures with "
              "different types");
          convert->priv->result = FALSE;
          return;
        }

        if (convert->priv->in_tex_formats[j] != in_tex->tex_format)
          tex_format_change = TRUE;

        convert->priv->in_tex_formats[j] = in_tex->tex_format;
      }
    }

    if (tex_format_change)
      gst_gl_color_convert_reset_shader (convert);
  }

  if (GST_VIDEO_FORMAT_INFO_IS_TILED (convert->in_info.finfo)) {
    GstVideoMeta *vmeta = gst_buffer_get_video_meta (convert->inbuf);
    gsize stride;

    stride = GST_VIDEO_INFO_PLANE_STRIDE (&convert->in_info, 0);

    if (vmeta && vmeta->stride[0] != stride) {
      GST_VIDEO_INFO_PLANE_STRIDE (&convert->in_info, 0) = vmeta->stride[0];
      gst_gl_color_convert_reset_shader (convert);
    }
  }

  if (!_init_convert (convert)) {
    convert->priv->result = FALSE;
    return;
  }

  sync_meta = gst_buffer_get_gl_sync_meta (convert->inbuf);
  if (sync_meta)
    gst_gl_sync_meta_wait (sync_meta, convert->context);

  if (!convert->priv->pool) {
    gboolean ret;
    /* No pool! */
    GstQuery *query = gst_query_new_allocation (convert->priv->out_caps, TRUE);
    ret = gst_gl_color_convert_decide_allocation (convert, query);
    gst_query_unref (query);

    if (!ret) {
      GST_ERROR_OBJECT (convert, "Failed to choose allocation parameters");
      convert->priv->result = FALSE;
      return;
    }

    if (!convert->priv->pool) {
      GST_ERROR_OBJECT (convert, "Failed to create a buffer pool");
      convert->priv->result = FALSE;
      return;
    }
  }

  if (!convert->priv->pool_started) {
    if (!gst_buffer_pool_set_active (convert->priv->pool, TRUE)) {
      GST_ERROR_OBJECT (convert, "Failed to start buffer pool");
      convert->priv->result = FALSE;
      return;
    }
    convert->priv->pool_started = TRUE;
  }

  ret =
      gst_buffer_pool_acquire_buffer (convert->priv->pool, &convert->outbuf,
      NULL);
  if (ret != GST_FLOW_OK) {
    GST_ERROR_OBJECT (convert, "Failed to acquire buffer from pool: %s",
        gst_flow_get_name (ret));
    convert->priv->result = FALSE;
    return;
  }

  gst_gl_insert_debug_marker (context, "%s converting from %s to %s",
      GST_OBJECT_NAME (convert),
      gst_video_format_to_string (GST_VIDEO_INFO_FORMAT (in_info)),
      gst_video_format_to_string (GST_VIDEO_INFO_FORMAT (&convert->out_info)));
  /* Handle all views on input and output one at a time */
  for (v = 0; res && v < views; v++)
    res = _do_convert_one_view (context, convert, v);

  if (!res) {
    gst_buffer_unref (convert->outbuf);
    convert->outbuf = NULL;
  }

  if (convert->outbuf) {
    GstVideoOverlayCompositionMeta *composition_meta;
    GstGLSyncMeta *sync_meta;

    if (G_UNLIKELY (!gst_buffer_is_writable (convert->outbuf))) {
      GST_WARNING_OBJECT (convert,
          "buffer is not writable at this point, bailing out");
      convert->priv->result = FALSE;
      return;
    }

    sync_meta = gst_buffer_get_gl_sync_meta (convert->outbuf);
    if (!sync_meta) {
      sync_meta = gst_buffer_add_gl_sync_meta (convert->context,
          convert->outbuf);
    }
    gst_gl_sync_meta_set_sync_point (sync_meta, convert->context);

    composition_meta =
        gst_buffer_get_video_overlay_composition_meta (convert->inbuf);
    if (composition_meta) {
      GST_DEBUG ("found video overlay composition meta, applying on output.");
      gst_buffer_add_video_overlay_composition_meta
          (convert->outbuf, composition_meta->overlay);
    }
  }

  convert->priv->result = res;
  return;
}

static gboolean
_do_convert_draw (GstGLContext * context, GstGLColorConvert * convert)
{
  GstGLFuncs *gl;
  struct ConvertInfo *c_info = &convert->priv->convert_info;
  guint out_width, out_height;
  gint i;
  gboolean ret = TRUE;

  GLenum multipleRT[] = {
    GL_COLOR_ATTACHMENT0,
    GL_COLOR_ATTACHMENT1,
    GL_COLOR_ATTACHMENT2,
    GL_COLOR_ATTACHMENT3
  };

  gl = context->gl_vtable;

  gst_gl_framebuffer_bind (convert->fbo);

  /* attach the texture to the FBO to renderer to */
  for (i = 0; i < c_info->out_n_textures; i++) {
    GstGLBaseMemory *tex = (GstGLBaseMemory *) convert->priv->out_tex[i];

    gst_gl_framebuffer_attach (convert->fbo, GL_COLOR_ATTACHMENT0 + i, tex);
  }

  if (gl->DrawBuffers)
    gl->DrawBuffers (c_info->out_n_textures, multipleRT);
  else if (gl->DrawBuffer)
    gl->DrawBuffer (GL_COLOR_ATTACHMENT0);

  gst_gl_framebuffer_get_effective_dimensions (convert->fbo, &out_width,
      &out_height);
  gl->Viewport (0, 0, out_width, out_height);

  gst_gl_shader_use (convert->shader);

  if (gl->BindVertexArray)
    gl->BindVertexArray (convert->priv->vao);
  _bind_buffer (convert);

  for (i = c_info->in_n_textures - 1; i >= 0; i--) {
    gchar *scale_name = g_strdup_printf ("tex_scale%u", i);
    guint gl_target =
        gst_gl_texture_target_to_gl (convert->priv->from_texture_target);

    gl->ActiveTexture (GL_TEXTURE0 + i);
    gl->BindTexture (gl_target, convert->priv->in_tex[i]->tex_id);
    gl->TexParameteri (gl_target, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl->TexParameteri (gl_target, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl->TexParameteri (gl_target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl->TexParameteri (gl_target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    gst_gl_shader_set_uniform_2fv (convert->shader, scale_name, 1,
        convert->priv->in_tex[i]->tex_scaling);

    g_free (scale_name);
  }

  gl->DrawElements (GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, 0);

  if (gl->BindVertexArray)
    gl->BindVertexArray (0);
  else
    _unbind_buffer (convert);

  if (gl->DrawBuffer)
    gl->DrawBuffer (GL_COLOR_ATTACHMENT0);

  /* we are done with the shader */
  gst_gl_context_clear_shader (context);

  if (!gst_gl_context_check_framebuffer_status (context, GL_FRAMEBUFFER))
    ret = FALSE;

  gst_gl_context_clear_framebuffer (context);

  return ret;
}

/**
 * gst_gl_color_convert_swizzle_shader_string:
 * @context: a #GstGLContext
 *
 * Returns: (transfer full): a shader string that can be used to swizzle vec
 * components in a GLSL shader.
 *
 * Since: 1.24
 */
gchar *
gst_gl_color_convert_swizzle_shader_string (GstGLContext * context)
{
  return g_strdup (glsl_func_swizzle);
}

/* *INDENT-OFF* */
static const char glsl_func_yuv_to_rgb[] =
    "vec3 yuv_to_rgb (vec3 yuv, vec3 offset, vec3 ycoeff, vec3 ucoeff, vec3 vcoeff) {\n"
    "  vec3 rgb;\n"
    "  yuv += offset;\n"
    "  rgb.r = dot(yuv, ycoeff);\n"
    "  rgb.g = dot(yuv, ucoeff);\n"
    "  rgb.b = dot(yuv, vcoeff);\n"
    "  return rgb;\n"
    "}\n";
/* *INDENT-ON* */

/**
 * gst_gl_color_convert_yuv_to_rgb_shader_string:
 * @context: a #GstGLContext
 *
 * The returned glsl function has declaration:
 *
 * `vec3 yuv_to_rgb (vec3 rgb, vec3 offset, vec3 ycoeff, vec3 ucoeff, vec3 vcoeff);`
 *
 * The Y component is placed in the 0th index of the returned value, The U component in the
 * 1st, and the V component in the 2nd.  offset, ycoeff, ucoeff, and vcoeff are the
 * specific coefficients and offset used for the conversion.
 *
 * Returns: (transfer full): a glsl function that can be used to convert from
 * yuv to rgb
 *
 * Since: 1.24
 */
gchar *
gst_gl_color_convert_yuv_to_rgb_shader_string (GstGLContext * context)
{
  return g_strdup (glsl_func_yuv_to_rgb);
}
