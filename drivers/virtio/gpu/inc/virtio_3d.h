// Copyright (c) 2018 Himanshu Goel
// 
// This software is released under the MIT License.
// https://opensource.org/licenses/MIT

#ifndef CARDINAL_VIRTIO_3D_H
#define CARDINAL_VIRTIO_3D_H

#include <stdint.h>
#include <types.h>

#define VREND_MAX_CTX (16)

typedef enum {
   VIRGL_OBJECT_NULL,
   VIRGL_OBJECT_BLEND,
   VIRGL_OBJECT_RASTERIZER,
   VIRGL_OBJECT_DSA,
   VIRGL_OBJECT_SHADER,
   VIRGL_OBJECT_VERTEX_ELEMENTS,
   VIRGL_OBJECT_SAMPLER_VIEW,
   VIRGL_OBJECT_SAMPLER_STATE,
   VIRGL_OBJECT_SURFACE,
   VIRGL_OBJECT_QUERY,
   VIRGL_OBJECT_STREAMOUT_TARGET,
   VIRGL_MAX_OBJECTS,
} virgl_object_type_t;

typedef enum {
   VIRGL_CCMD_NOP = 0,
   VIRGL_CCMD_CREATE_OBJECT = 1,
   VIRGL_CCMD_BIND_OBJECT,
   VIRGL_CCMD_DESTROY_OBJECT,
   VIRGL_CCMD_SET_VIEWPORT_STATE,
   VIRGL_CCMD_SET_FRAMEBUFFER_STATE,
   VIRGL_CCMD_SET_VERTEX_BUFFERS,
   VIRGL_CCMD_CLEAR,
   VIRGL_CCMD_DRAW_VBO,
   VIRGL_CCMD_RESOURCE_INLINE_WRITE,
   VIRGL_CCMD_SET_SAMPLER_VIEWS,
   VIRGL_CCMD_SET_INDEX_BUFFER,
   VIRGL_CCMD_SET_CONSTANT_BUFFER,
   VIRGL_CCMD_SET_STENCIL_REF,
   VIRGL_CCMD_SET_BLEND_COLOR,
   VIRGL_CCMD_SET_SCISSOR_STATE,
   VIRGL_CCMD_BLIT,
   VIRGL_CCMD_RESOURCE_COPY_REGION,
   VIRGL_CCMD_BIND_SAMPLER_STATES,
   VIRGL_CCMD_BEGIN_QUERY,
   VIRGL_CCMD_END_QUERY,
   VIRGL_CCMD_GET_QUERY_RESULT,
   VIRGL_CCMD_SET_POLYGON_STIPPLE,
   VIRGL_CCMD_SET_CLIP_STATE,
   VIRGL_CCMD_SET_SAMPLE_MASK,
   VIRGL_CCMD_SET_STREAMOUT_TARGETS,
   VIRGL_CCMD_SET_RENDER_CONDITION,
   VIRGL_CCMD_SET_UNIFORM_BUFFER,

   VIRGL_CCMD_SET_SUB_CTX,
   VIRGL_CCMD_CREATE_SUB_CTX,
   VIRGL_CCMD_DESTROY_SUB_CTX,
   VIRGL_CCMD_BIND_SHADER
} virtio_virgl_ccmd_t;

//LSB FIRST
typedef struct {
  uint8_t depth   : 1;
  uint8_t stencil : 1;
  uint8_t color0  : 1;
  uint8_t color1  : 1;
  uint8_t color2  : 1;
  uint8_t color3  : 1;
  uint8_t color4  : 1;
  uint8_t color5  : 1;
  uint8_t color6  : 1;
  uint8_t color7  : 1;
} clear_type_t;

typedef struct PACKED {
    clear_type_t clear_type;
    uint32_t R;
    uint32_t G;
    uint32_t B;
    uint32_t A;
    uint64_t Depth;
    uint32_t stencil;
} ccmd_clear_t;

typedef struct PACKED {
    uint32_t start;
    uint32_t count;
    
    uint32_t mode;
    uint32_t indexed;

    uint32_t instance_count;
    uint32_t index_bias;

    uint32_t start_instance;
    uint32_t primitive_restart;
    uint32_t restart_index;

    uint32_t min_index;
    uint32_t max_index;

    uint32_t cso;
} ccmd_draw_vbo_t;

#endif