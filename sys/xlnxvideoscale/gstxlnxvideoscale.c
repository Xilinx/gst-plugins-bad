/* GStreamer
 * Copyright (C) <1999> Erik Walthinsen <omega@cse.ogi.edu>
 * Copyright (C) 2005-2012 David Schleef <ds@schleef.org>
 * Copyright (C) 2019 Naveen Cherukuri <naveenc@xilinx.com>
 *                    Saurabh Sengar <saurabh.singh@xilinx.com>
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

/**
 * SECTION:xlnxvideoscale
 *
 * This plugin does scaling & color conversion using Xilinx's VPSS IP
 * As Xilinx VPSS IP is streaming based IP, this uses talks to FBRead 
 * and FBWrite IPs to send & receive frames from VPSS IP.
 *              +----------------------------+
 *             -|        xlnxvideoscale      |- 
 *              +----------------------------+
 *                |                       ^
 *                |                       |
 *                V                       |
 *            +--------+   +------+   +---------+
 *            | FBRead |-->| VPSS |-->| FBWrite |
 *            +--------+   +------+   +---------+
 *                
 * <refsect2>
 * <title>Example launch line</title>
 * |[
 * gst-launch -v videotestsrc ! video/x-raw, width=1920, height=1080, format=YUY2 ! \n
 *     xlnxvideoscale ! video/x-raw, width=1280, height=720, format=BGR ! fakesink 
 * ]|
 * </refsect2>
 */

 // VPSS & FB Drivercode : https://github.com/Xilinx/linux-xlnx/tree/master/drivers/staging/xlnx_ctrl_driver
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "gstxlnxvideoscale.h"
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <gst/allocators/gstdmabuf.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define GST_CAT_DEFAULT xlnx_video_scale_debug
GST_DEBUG_CATEGORY_STATIC (xlnx_video_scale_debug);
GST_DEBUG_CATEGORY_STATIC (CAT_PERFORMANCE);

#define XSET_FB_CAPTURE      16
#define XSET_FB_CONFIGURE    17
#define XSET_FB_ENABLE       18
#define XSET_FB_DISABLE      19
#define XSET_FB_RELEASE      20
#define XSET_FB_ENABLE_SNGL  21
#define XSET_FB_POLL         22

#define XVPSS_SET_CONFIGURE  16
#define XVPSS_SET_ENABLE     17
#define XVPSS_SET_DISABLE    18

struct frmb_data
{
  unsigned int fd;
  unsigned int height;
  unsigned int width;
  unsigned int stride;
  unsigned int color;
  unsigned int n_planes;
  unsigned int offset;
  unsigned int is_wait;
};

struct xvpss_data
{
  unsigned int height_in;
  unsigned int width_in;
  unsigned int height_out;
  unsigned int width_out;
  unsigned int color_in;
  unsigned int color_out;
};

#define XLNX_VIDEO_SCALE_CAPS \
    "video/x-raw, " \
    "format = (string) {YUY2, UYVY, NV12, NV16, RGB, BGR, xRGB, GRAY8}, " \
    "width = (int) [ 1, 3840 ], " \
    "height = (int) [ 1, 2160 ], " \
    "framerate = " GST_VIDEO_FPS_RANGE

static GstStaticPadTemplate src_template = GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS (XLNX_VIDEO_SCALE_CAPS)
    );

static GstStaticPadTemplate sink_template = GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS (XLNX_VIDEO_SCALE_CAPS)
    );

static GstCaps *gst_xlnx_video_scale_transform_caps (GstBaseTransform * trans,
    GstPadDirection direction, GstCaps * caps, GstCaps * filter);
static GstCaps *gst_xlnx_video_scale_fixate_caps (GstBaseTransform * base,
    GstPadDirection direction, GstCaps * caps, GstCaps * othercaps);
static GstFlowReturn gst_xlnx_video_scale_transform_frame (GstVideoFilter *
    filter, GstVideoFrame * in, GstVideoFrame * out);
static gboolean gst_xlnx_video_scale_set_info (GstVideoFilter * filter,
    GstCaps * in, GstVideoInfo * in_info, GstCaps * out,
    GstVideoInfo * out_info);
static GstStateChangeReturn gst_xlnx_video_scale_change_state (GstElement *
    element, GstStateChange transition);

#define gst_xlnx_video_scale_parent_class parent_class
G_DEFINE_TYPE (GstXlnxVideoScale, gst_xlnx_video_scale, GST_TYPE_VIDEO_FILTER);

#define XLNX_VIDEO_SCALE_VPSS_NODE "/dev/xvpss"
#define XLNX_VIDEO_SCALE_FB_WRITE_NODE "/dev/fbwr"
#define XLNX_VIDEO_SCALE_FB_READ_NODE "/dev/fbrd"

#define CHECK_ERROR(func, gstobj, ret_val) do { \
  int iret = (func); \
  if (iret < 0) { \
    GST_ERROR_OBJECT(gstobj, "ioctl failed. error : %s", g_strerror(errno)); \
    return ret_val; \
  } \
} while (0)

#define XILINX_FRMBUF_FMT_RGBX8      10
#define XILINX_FRMBUF_FMT_YUVX8      11
#define XILINX_FRMBUF_FMT_YUYV8      12
#define XILINX_FRMBUF_FMT_RGBA8      13
#define XILINX_FRMBUF_FMT_YUVA8      14
#define XILINX_FRMBUF_FMT_RGBX10     15
#define XILINX_FRMBUF_FMT_YUVX10     16
#define XILINX_FRMBUF_FMT_Y_UV8      18
#define XILINX_FRMBUF_FMT_Y_UV8_420  19
#define XILINX_FRMBUF_FMT_RGB8       20
#define XILINX_FRMBUF_FMT_YUV8       21
#define XILINX_FRMBUF_FMT_Y_UV10     22
#define XILINX_FRMBUF_FMT_Y_UV10_420 23
#define XILINX_FRMBUF_FMT_Y8         24
#define XILINX_FRMBUF_FMT_Y10        25
#define XILINX_FRMBUF_FMT_BGRA8      26
#define XILINX_FRMBUF_FMT_BGRX8      27
#define XILINX_FRMBUF_FMT_UYVY8      28
#define XILINX_FRMBUF_FMT_BGR8       29
#define XILINX_FRMBUF_FMT_RGBX12     30
#define XILINX_FRMBUF_FMT_RGB16      35

enum
{
  XVIDC_CSF_RGB = 0,
  XVIDC_CSF_YCRCB_444,
  XVIDC_CSF_YCRCB_422,
  XVIDC_CSF_YCRCB_420,
  XVIDC_CSF_NOT_SUPPORTED
};

static unsigned int
get_xilinx_framebuf_format (GstVideoFormat gst_fourcc)
{
  switch (gst_fourcc) {
    case GST_VIDEO_FORMAT_YUY2:
      return XILINX_FRMBUF_FMT_YUYV8;
    case GST_VIDEO_FORMAT_UYVY:
      return XILINX_FRMBUF_FMT_UYVY8;
    case GST_VIDEO_FORMAT_NV12:
      return XILINX_FRMBUF_FMT_Y_UV8_420;
    case GST_VIDEO_FORMAT_NV16:
      return XILINX_FRMBUF_FMT_Y_UV8;
    case GST_VIDEO_FORMAT_RGB:
      return XILINX_FRMBUF_FMT_RGB8;
    case GST_VIDEO_FORMAT_BGR:
      return XILINX_FRMBUF_FMT_BGR8;
    case GST_VIDEO_FORMAT_xRGB:
      return XILINX_FRMBUF_FMT_BGRX8;
    case GST_VIDEO_FORMAT_GRAY8:
      return XILINX_FRMBUF_FMT_Y8;
    default:
      return 0;
  }
}

static unsigned
get_xilinx_vpss_format (GstVideoFormat gst_fourcc)
{
  switch (gst_fourcc) {
    case GST_VIDEO_FORMAT_RGB:
    case GST_VIDEO_FORMAT_BGR:
    case GST_VIDEO_FORMAT_xRGB:
      return XVIDC_CSF_RGB;
    case GST_VIDEO_FORMAT_GRAY8:
      return XVIDC_CSF_YCRCB_444;
    case GST_VIDEO_FORMAT_NV16:
    case GST_VIDEO_FORMAT_UYVY:
    case GST_VIDEO_FORMAT_YUY2:
      return XVIDC_CSF_YCRCB_422;
    case GST_VIDEO_FORMAT_NV12:
      return XVIDC_CSF_YCRCB_420;
    default:
      return XVIDC_CSF_NOT_SUPPORTED;
  }
}

static inline void
gst_xlnx_video_scale_time_division_lock (GstXlnxVideoScale * videoscale)
{
  GstXlnxVideoScaleClass *klass = (GstXlnxVideoScaleClass *)
      GST_XLNX_VIDEO_SCALE_GET_CLASS (G_OBJECT (videoscale));

  GST_LOG_OBJECT (videoscale, "acquiring time division lock : %p",
      &klass->time_division_lock);
  g_mutex_lock (&klass->time_division_lock);
}

static inline void
gst_xlnx_video_scale_time_division_unlock (GstXlnxVideoScale * videoscale)
{
  GstXlnxVideoScaleClass *klass =
      (GstXlnxVideoScaleClass *) GST_XLNX_VIDEO_SCALE_GET_CLASS (videoscale);

  GST_LOG_OBJECT (videoscale, "releasing time division lock : %p",
      &klass->time_division_lock);
  g_mutex_unlock (&klass->time_division_lock);
}

static void
gst_xlnx_video_scale_class_init (GstXlnxVideoScaleClass * klass)
{
  GstElementClass *element_class = (GstElementClass *) klass;
  GstBaseTransformClass *trans_class = (GstBaseTransformClass *) klass;
  GstVideoFilterClass *filter_class = (GstVideoFilterClass *) klass;

  gst_element_class_set_static_metadata (element_class,
      "Xilinx Video scaler", "Filter/Converter/Video/Scaler",
      "Scaling & Color conversion video using VPSS IP",
      "Naveen Cherukuri <naveenc@xilinx.com>, "
      "Saurabh Sengar <saurabh.singh@xilinx.com>");

  gst_element_class_add_static_pad_template (element_class, &sink_template);
  gst_element_class_add_static_pad_template (element_class, &src_template);

  element_class->change_state =
      GST_DEBUG_FUNCPTR (gst_xlnx_video_scale_change_state);

  trans_class->transform_caps =
      GST_DEBUG_FUNCPTR (gst_xlnx_video_scale_transform_caps);
  trans_class->fixate_caps =
      GST_DEBUG_FUNCPTR (gst_xlnx_video_scale_fixate_caps);
  filter_class->set_info = GST_DEBUG_FUNCPTR (gst_xlnx_video_scale_set_info);
  filter_class->transform_frame =
      GST_DEBUG_FUNCPTR (gst_xlnx_video_scale_transform_frame);
}

static void
gst_xlnx_video_scale_init (GstXlnxVideoScale * videoscale)
{
}

static GstStateChangeReturn
gst_xlnx_video_scale_change_state (GstElement * element,
    GstStateChange transition)
{
  GstStateChangeReturn ret;
  GstXlnxVideoScale *vscale = GST_XLNX_VIDEO_SCALE (element);
  GST_DEBUG_OBJECT (vscale, "changing state: %s => %s",
      gst_element_state_get_name (GST_STATE_TRANSITION_CURRENT (transition)),
      gst_element_state_get_name (GST_STATE_TRANSITION_NEXT (transition)));

  switch (transition) {
    case GST_STATE_CHANGE_NULL_TO_READY:{
      /* open xilinx fb read driver */
      vscale->fbrd_fd = open (XLNX_VIDEO_SCALE_FB_READ_NODE, O_RDWR);
      if (vscale->fbrd_fd < 0) {
        GST_ERROR_OBJECT (vscale, "failed to open fb read driver : %s",
            XLNX_VIDEO_SCALE_FB_READ_NODE);
        return GST_STATE_CHANGE_FAILURE;
      }

      /* open xilinx fb write driver */
      vscale->fbwr_fd = open (XLNX_VIDEO_SCALE_FB_WRITE_NODE, O_RDWR);
      if (vscale->fbwr_fd < 0) {
        GST_ERROR_OBJECT (vscale, "failed to open fb read driver : %s",
            XLNX_VIDEO_SCALE_FB_WRITE_NODE);
        return GST_STATE_CHANGE_FAILURE;
      }

      /* open xilinx vpss driver */
      vscale->vpss_fd = open (XLNX_VIDEO_SCALE_VPSS_NODE, O_RDWR);
      if (vscale->vpss_fd < 0) {
        GST_ERROR_OBJECT (vscale, "failed to open vpss driver : %s",
            XLNX_VIDEO_SCALE_VPSS_NODE);
        return GST_STATE_CHANGE_FAILURE;
      }
      GST_LOG_OBJECT (vscale, "opened fds : fbrd = %u, fdvpss = %u, fbwr = %u",
          vscale->fbrd_fd, vscale->vpss_fd, vscale->fbwr_fd);
      break;
    }
    default:
      break;
  }

  ret = GST_ELEMENT_CLASS (parent_class)->change_state (element, transition);

  switch (transition) {
    case GST_STATE_CHANGE_READY_TO_NULL:{
      close (vscale->fbrd_fd);
      close (vscale->vpss_fd);
      close (vscale->fbwr_fd);
      GST_LOG_OBJECT (vscale, "closed fds : fbrd = %u, fdvpss = %u, fbwr = %u",
          vscale->fbrd_fd, vscale->vpss_fd, vscale->fbwr_fd);
      break;
    }
    default:
      break;
  }

  return ret;
}

static gboolean
gst_xlnx_video_scale_set_info (GstVideoFilter * filter, GstCaps * in,
    GstVideoInfo * in_info, GstCaps * out, GstVideoInfo * out_info)
{
  if (in_info->width == out_info->width && in_info->height == out_info->height
      && GST_VIDEO_INFO_FORMAT (in_info) == GST_VIDEO_INFO_FORMAT (out_info)) {
    GST_INFO_OBJECT (filter, "enabling pass through mode");
    gst_base_transform_set_passthrough (GST_BASE_TRANSFORM (filter), TRUE);
  } else {
    gst_base_transform_set_passthrough (GST_BASE_TRANSFORM (filter), FALSE);
  }
  return TRUE;
}

static GstCaps *
gst_xlnx_video_scale_transform_caps (GstBaseTransform * trans,
    GstPadDirection direction, GstCaps * caps, GstCaps * filter)
{
  GstCaps *ret;
  GstStructure *structure;
  GstCapsFeatures *features;
  gint i, n;

  GST_DEBUG_OBJECT (trans,
      "Transforming caps %" GST_PTR_FORMAT " in direction %s", caps,
      (direction == GST_PAD_SINK) ? "sink" : "src");

  ret = gst_caps_new_empty ();
  n = gst_caps_get_size (caps);
  for (i = 0; i < n; i++) {
    structure = gst_caps_get_structure (caps, i);
    features = gst_caps_get_features (caps, i);

    /* If this is already expressed by the existing caps
     * skip this structure */
    if (i > 0 && gst_caps_is_subset_structure_full (ret, structure, features))
      continue;

    /* make copy */
    structure = gst_structure_copy (structure);

    /* If the features are non-sysmem we can only do passthrough */
    if (!gst_caps_features_is_any (features)
        && gst_caps_features_is_equal (features,
            GST_CAPS_FEATURES_MEMORY_SYSTEM_MEMORY)) {
      gst_structure_set (structure, "width", GST_TYPE_INT_RANGE, 1, G_MAXINT,
          "height", GST_TYPE_INT_RANGE, 1, G_MAXINT, NULL);

      gst_structure_remove_fields (structure, "format", "colorimetry",
          "chroma-site", NULL);

      /* if pixel aspect ratio, make a range of it */
      if (gst_structure_has_field (structure, "pixel-aspect-ratio")) {
        gst_structure_set (structure, "pixel-aspect-ratio",
            GST_TYPE_FRACTION_RANGE, 1, G_MAXINT, G_MAXINT, 1, NULL);
      }
    }
    gst_caps_append_structure_full (ret, structure,
        gst_caps_features_copy (features));
  }

  if (filter) {
    GstCaps *intersection;

    intersection =
        gst_caps_intersect_full (filter, ret, GST_CAPS_INTERSECT_FIRST);
    gst_caps_unref (ret);
    ret = intersection;
  }

  GST_DEBUG_OBJECT (trans, "returning caps: %" GST_PTR_FORMAT, ret);

  return ret;
}

static gboolean
xlnx_video_scale_register_dmabuf (GstXlnxVideoScale * videoscale,
    GstVideoFrame * frame, gint fb_fd, gboolean is_input)
{
  GstXlnxVideoScaleClass *klass = (GstXlnxVideoScaleClass *)
      GST_XLNX_VIDEO_SCALE_GET_CLASS (G_OBJECT (videoscale));
  struct frmb_data data = { 0, };
  GstMemory *mem = NULL;
  gint dma_fd;
  GstVideoMeta *vmeta = NULL;
  GstVideoInfo *cur_vinfo = is_input ? &klass->in_vinfo : &klass->out_vinfo;

  mem = gst_buffer_get_memory (frame->buffer, 0);
  if (!gst_is_dmabuf_memory (mem)) {
    GST_ERROR_OBJECT (videoscale, "buffer is NOT a dmabuf");
    return FALSE;
  }
  dma_fd = gst_dmabuf_memory_get_fd (mem);
  if (dma_fd == -1) {
    GST_ERROR_OBJECT (videoscale, "failed to get DMA buffer fd");
    gst_memory_unref (mem);
    return FALSE;
  }
  gst_memory_unref (mem);

  /* VCU sends info in meta */
  vmeta = gst_buffer_get_video_meta (frame->buffer);
  if (vmeta == NULL) {
    GST_INFO_OBJECT (videoscale, "video meta not present in buffer");
  }

  data.fd = dma_fd;
  data.n_planes = GST_VIDEO_FRAME_N_PLANES (frame);
  if (data.n_planes == 2) {
    if (vmeta)
      data.offset = vmeta->offset[1];
    else
      data.offset = GST_VIDEO_FRAME_PLANE_OFFSET (frame, 1);
  } else if (data.n_planes > 2) {
    GST_ERROR_OBJECT (videoscale, "num planes > 2 not supported : %d",
        data.n_planes);
    return FALSE;
  }

  if (GST_VIDEO_FRAME_HEIGHT (frame) != GST_VIDEO_INFO_HEIGHT (cur_vinfo) ||
      GST_VIDEO_FRAME_WIDTH (frame) != GST_VIDEO_INFO_WIDTH (cur_vinfo) ||
      GST_VIDEO_FRAME_FORMAT (frame) != GST_VIDEO_INFO_FORMAT (cur_vinfo)) {

    GST_INFO_OBJECT (videoscale, "need to configure frame-buffer %s",
        is_input ? "read" : "write");

    if (data.n_planes == 1) {
      data.height = GST_VIDEO_FRAME_HEIGHT (frame);
      data.width = GST_VIDEO_FRAME_WIDTH (frame);
      data.stride = GST_VIDEO_FRAME_PLANE_STRIDE (frame, 0);
    } else if (data.n_planes == 2) {
      if (vmeta) {
        data.height = vmeta->height;
        data.width = vmeta->width;
        data.stride = vmeta->stride[0];
      } else {
        data.height = GST_VIDEO_FRAME_HEIGHT (frame);
        data.width = GST_VIDEO_FRAME_WIDTH (frame);
        data.stride = GST_VIDEO_FRAME_PLANE_STRIDE (frame, 0);
      }
    } else if (data.n_planes > 2) {
      GST_ERROR_OBJECT (videoscale, "num planes > 2 not supported : %d",
          data.n_planes);
      return FALSE;
    }

    data.color = get_xilinx_framebuf_format (GST_VIDEO_FRAME_FORMAT (frame));
    if (!data.color) {
      GST_ERROR_OBJECT (videoscale, "unsupported fourcc");
      return FALSE;
    }

    /* cache video info */
    if (is_input) {
      gst_video_info_set_format (&klass->in_vinfo,
          GST_VIDEO_FRAME_FORMAT (frame), GST_VIDEO_FRAME_WIDTH (frame),
          GST_VIDEO_FRAME_HEIGHT (frame));
    } else {
      gst_video_info_set_format (&klass->out_vinfo,
          GST_VIDEO_FRAME_FORMAT (frame), GST_VIDEO_FRAME_WIDTH (frame),
          GST_VIDEO_FRAME_HEIGHT (frame));
    }
    GST_DEBUG_OBJECT (videoscale,
        "configuring FB%s : w = %d, h = %d, stride = %d, offset = %d, fourcc = %d, dmafd = %d",
        is_input ? "Read" : "Write", data.width, data.height, data.stride,
        data.offset, data.color, data.fd);

    CHECK_ERROR (ioctl (fb_fd, XSET_FB_CONFIGURE, &data), videoscale, FALSE);
  }

  /* fetches physical address corresponding to a dmabuf fd */
  CHECK_ERROR (ioctl (fb_fd, XSET_FB_CAPTURE, &data), videoscale, FALSE);

  /* enables FB write IP */
  CHECK_ERROR (ioctl (fb_fd, XSET_FB_ENABLE_SNGL, NULL), videoscale, FALSE);

  GST_LOG_OBJECT (videoscale, "successfully registered fd = %d with FB%s",
      data.fd, is_input ? "Read" : "Write");

  return TRUE;
}

static gboolean
xlnx_video_scale_unregister_dmabuf (GstXlnxVideoScale * videoscale,
    GstVideoFrame * frame, gint fb_fd)
{
  CHECK_ERROR (ioctl (fb_fd, XSET_FB_RELEASE, NULL), videoscale, FALSE);
  CHECK_ERROR (ioctl (fb_fd, XSET_FB_DISABLE, NULL), videoscale, FALSE);

  return TRUE;
}

static gboolean
xlnx_video_scale_configure_vpss (GstXlnxVideoScale * videoscale,
    GstVideoFrame * in_frame, GstVideoFrame * out_frame)
{
  struct xvpss_data vpss_data;
  unsigned int fourcc = 0;
  GstXlnxVideoScaleClass *klass = (GstXlnxVideoScaleClass *)
      GST_XLNX_VIDEO_SCALE_GET_CLASS (G_OBJECT (videoscale));

  GST_DEBUG_OBJECT (videoscale, "input format = %s and output format = %s",
      gst_video_format_to_string (GST_VIDEO_FRAME_FORMAT (in_frame)),
      gst_video_format_to_string (GST_VIDEO_FRAME_FORMAT (out_frame)));

  if (GST_VIDEO_FRAME_HEIGHT (in_frame) !=
      GST_VIDEO_INFO_HEIGHT (&klass->in_vinfo)
      || GST_VIDEO_FRAME_WIDTH (in_frame) !=
      GST_VIDEO_INFO_WIDTH (&klass->in_vinfo)
      || GST_VIDEO_FRAME_FORMAT (in_frame) !=
      GST_VIDEO_INFO_FORMAT (&klass->in_vinfo)
      || GST_VIDEO_FRAME_HEIGHT (out_frame) !=
      GST_VIDEO_INFO_HEIGHT (&klass->out_vinfo)
      || GST_VIDEO_FRAME_WIDTH (out_frame) !=
      GST_VIDEO_INFO_WIDTH (&klass->out_vinfo)
      || GST_VIDEO_FRAME_FORMAT (out_frame) !=
      GST_VIDEO_INFO_FORMAT (&klass->out_vinfo)) {

    GST_INFO_OBJECT (videoscale, "need to configure VPSS");

    /* configure VPSS */
    vpss_data.height_in = GST_VIDEO_FRAME_HEIGHT (in_frame);
    vpss_data.width_in = GST_VIDEO_FRAME_WIDTH (in_frame);
    vpss_data.height_out = GST_VIDEO_FRAME_HEIGHT (out_frame);
    vpss_data.width_out = GST_VIDEO_FRAME_WIDTH (out_frame);
    fourcc = get_xilinx_vpss_format (GST_VIDEO_FRAME_FORMAT (in_frame));
    if (fourcc == XVIDC_CSF_NOT_SUPPORTED) {
      GST_ERROR_OBJECT (videoscale, "unsupported VPSS input format");
      return FALSE;
    }
    vpss_data.color_in = fourcc;

    fourcc = get_xilinx_vpss_format (GST_VIDEO_FRAME_FORMAT (out_frame));
    if (fourcc == XVIDC_CSF_NOT_SUPPORTED) {
      GST_ERROR_OBJECT (videoscale, "unsupported VPSS output format");
      return FALSE;
    }
    vpss_data.color_out = fourcc;

    /* configures VPSS IP */
    CHECK_ERROR (ioctl (videoscale->vpss_fd, XVPSS_SET_CONFIGURE, &vpss_data),
        videoscale, FALSE);
    /* enables VPSS IP */
    CHECK_ERROR (ioctl (videoscale->vpss_fd, XVPSS_SET_ENABLE, NULL),
        videoscale, FALSE);
  }

  return TRUE;
}

static GstCaps *
gst_xlnx_video_scale_fixate_caps (GstBaseTransform * base,
    GstPadDirection direction, GstCaps * caps, GstCaps * othercaps)
{
  GstStructure *ins, *outs;
  const GValue *from_par, *to_par;
  GValue fpar = { 0, }, tpar = {
  0,};

  othercaps = gst_caps_truncate (othercaps);
  othercaps = gst_caps_make_writable (othercaps);

  GST_DEBUG_OBJECT (base, "trying to fixate othercaps %" GST_PTR_FORMAT
      " based on caps %" GST_PTR_FORMAT, othercaps, caps);

  ins = gst_caps_get_structure (caps, 0);
  outs = gst_caps_get_structure (othercaps, 0);

  from_par = gst_structure_get_value (ins, "pixel-aspect-ratio");
  to_par = gst_structure_get_value (outs, "pixel-aspect-ratio");

  /* If we're fixating from the sinkpad we always set the PAR and
   * assume that missing PAR on the sinkpad means 1/1 and
   * missing PAR on the srcpad means undefined
   */
  if (direction == GST_PAD_SINK) {
    if (!from_par) {
      g_value_init (&fpar, GST_TYPE_FRACTION);
      gst_value_set_fraction (&fpar, 1, 1);
      from_par = &fpar;
    }
    if (!to_par) {
      g_value_init (&tpar, GST_TYPE_FRACTION_RANGE);
      gst_value_set_fraction_range_full (&tpar, 1, G_MAXINT, G_MAXINT, 1);
      to_par = &tpar;
    }
  } else {
    if (!to_par) {
      g_value_init (&tpar, GST_TYPE_FRACTION);
      gst_value_set_fraction (&tpar, 1, 1);
      to_par = &tpar;

      gst_structure_set (outs, "pixel-aspect-ratio", GST_TYPE_FRACTION, 1, 1,
          NULL);
    }
    if (!from_par) {
      g_value_init (&fpar, GST_TYPE_FRACTION);
      gst_value_set_fraction (&fpar, 1, 1);
      from_par = &fpar;
    }
  }

  /* we have both PAR but they might not be fixated */
  {
    gint from_w, from_h, from_par_n, from_par_d, to_par_n, to_par_d;
    gint w = 0, h = 0;
    gint from_dar_n, from_dar_d;
    gint num, den;

    /* from_par should be fixed */
    g_return_val_if_fail (gst_value_is_fixed (from_par), othercaps);

    from_par_n = gst_value_get_fraction_numerator (from_par);
    from_par_d = gst_value_get_fraction_denominator (from_par);

    gst_structure_get_int (ins, "width", &from_w);
    gst_structure_get_int (ins, "height", &from_h);

    gst_structure_get_int (outs, "width", &w);
    gst_structure_get_int (outs, "height", &h);

    /* if both width and height are already fixed, we can't do anything
     * about it anymore */
    if (w && h) {
      guint n, d;

      GST_DEBUG_OBJECT (base, "dimensions already set to %dx%d, not fixating",
          w, h);
      if (!gst_value_is_fixed (to_par)) {
        if (gst_video_calculate_display_ratio (&n, &d, from_w, from_h,
                from_par_n, from_par_d, w, h)) {
          GST_DEBUG_OBJECT (base, "fixating to_par to %dx%d", n, d);
          if (gst_structure_has_field (outs, "pixel-aspect-ratio"))
            gst_structure_fixate_field_nearest_fraction (outs,
                "pixel-aspect-ratio", n, d);
          else if (n != d)
            gst_structure_set (outs, "pixel-aspect-ratio", GST_TYPE_FRACTION,
                n, d, NULL);
        }
      }
      goto done;
    }

    /* Calculate input DAR */
    if (!gst_util_fraction_multiply (from_w, from_h, from_par_n, from_par_d,
            &from_dar_n, &from_dar_d)) {
      GST_ELEMENT_ERROR (base, CORE, NEGOTIATION, (NULL),
          ("Error calculating the output scaled size - integer overflow"));
      goto done;
    }

    GST_DEBUG_OBJECT (base, "Input DAR is %d/%d", from_dar_n, from_dar_d);

    /* If either width or height are fixed there's not much we
     * can do either except choosing a height or width and PAR
     * that matches the DAR as good as possible
     */
    if (h) {
      GstStructure *tmp;
      gint set_w, set_par_n, set_par_d;

      GST_DEBUG_OBJECT (base, "height is fixed (%d)", h);

      /* If the PAR is fixed too, there's not much to do
       * except choosing the width that is nearest to the
       * width with the same DAR */
      if (gst_value_is_fixed (to_par)) {
        to_par_n = gst_value_get_fraction_numerator (to_par);
        to_par_d = gst_value_get_fraction_denominator (to_par);

        GST_DEBUG_OBJECT (base, "PAR is fixed %d/%d", to_par_n, to_par_d);

        if (!gst_util_fraction_multiply (from_dar_n, from_dar_d, to_par_d,
                to_par_n, &num, &den)) {
          GST_ELEMENT_ERROR (base, CORE, NEGOTIATION, (NULL),
              ("Error calculating the output scaled size - integer overflow"));
          goto done;
        }

        w = (guint) gst_util_uint64_scale_int (h, num, den);
        gst_structure_fixate_field_nearest_int (outs, "width", w);

        goto done;
      }

      /* The PAR is not fixed and it's quite likely that we can set
       * an arbitrary PAR. */

      /* Check if we can keep the input width */
      tmp = gst_structure_copy (outs);
      gst_structure_fixate_field_nearest_int (tmp, "width", from_w);
      gst_structure_get_int (tmp, "width", &set_w);

      /* Might have failed but try to keep the DAR nonetheless by
       * adjusting the PAR */
      if (!gst_util_fraction_multiply (from_dar_n, from_dar_d, h, set_w,
              &to_par_n, &to_par_d)) {
        GST_ELEMENT_ERROR (base, CORE, NEGOTIATION, (NULL),
            ("Error calculating the output scaled size - integer overflow"));
        gst_structure_free (tmp);
        goto done;
      }

      if (!gst_structure_has_field (tmp, "pixel-aspect-ratio"))
        gst_structure_set_value (tmp, "pixel-aspect-ratio", to_par);
      gst_structure_fixate_field_nearest_fraction (tmp, "pixel-aspect-ratio",
          to_par_n, to_par_d);
      gst_structure_get_fraction (tmp, "pixel-aspect-ratio", &set_par_n,
          &set_par_d);
      gst_structure_free (tmp);

      /* Check if the adjusted PAR is accepted */
      if (set_par_n == to_par_n && set_par_d == to_par_d) {
        if (gst_structure_has_field (outs, "pixel-aspect-ratio") ||
            set_par_n != set_par_d)
          gst_structure_set (outs, "width", G_TYPE_INT, set_w,
              "pixel-aspect-ratio", GST_TYPE_FRACTION, set_par_n, set_par_d,
              NULL);
        goto done;
      }

      /* Otherwise scale the width to the new PAR and check if the
       * adjusted with is accepted. If all that fails we can't keep
       * the DAR */
      if (!gst_util_fraction_multiply (from_dar_n, from_dar_d, set_par_d,
              set_par_n, &num, &den)) {
        GST_ELEMENT_ERROR (base, CORE, NEGOTIATION, (NULL),
            ("Error calculating the output scaled size - integer overflow"));
        goto done;
      }

      w = (guint) gst_util_uint64_scale_int (h, num, den);
      gst_structure_fixate_field_nearest_int (outs, "width", w);
      if (gst_structure_has_field (outs, "pixel-aspect-ratio") ||
          set_par_n != set_par_d)
        gst_structure_set (outs, "pixel-aspect-ratio", GST_TYPE_FRACTION,
            set_par_n, set_par_d, NULL);

      goto done;
    } else if (w) {
      GstStructure *tmp;
      gint set_h, set_par_n, set_par_d;

      GST_DEBUG_OBJECT (base, "width is fixed (%d)", w);

      /* If the PAR is fixed too, there's not much to do
       * except choosing the height that is nearest to the
       * height with the same DAR */
      if (gst_value_is_fixed (to_par)) {
        to_par_n = gst_value_get_fraction_numerator (to_par);
        to_par_d = gst_value_get_fraction_denominator (to_par);

        GST_DEBUG_OBJECT (base, "PAR is fixed %d/%d", to_par_n, to_par_d);

        if (!gst_util_fraction_multiply (from_dar_n, from_dar_d, to_par_d,
                to_par_n, &num, &den)) {
          GST_ELEMENT_ERROR (base, CORE, NEGOTIATION, (NULL),
              ("Error calculating the output scaled size - integer overflow"));
          goto done;
        }

        h = (guint) gst_util_uint64_scale_int (w, den, num);
        gst_structure_fixate_field_nearest_int (outs, "height", h);

        goto done;
      }

      /* The PAR is not fixed and it's quite likely that we can set
       * an arbitrary PAR. */

      /* Check if we can keep the input height */
      tmp = gst_structure_copy (outs);
      gst_structure_fixate_field_nearest_int (tmp, "height", from_h);
      gst_structure_get_int (tmp, "height", &set_h);

      /* Might have failed but try to keep the DAR nonetheless by
       * adjusting the PAR */
      if (!gst_util_fraction_multiply (from_dar_n, from_dar_d, set_h, w,
              &to_par_n, &to_par_d)) {
        GST_ELEMENT_ERROR (base, CORE, NEGOTIATION, (NULL),
            ("Error calculating the output scaled size - integer overflow"));
        gst_structure_free (tmp);
        goto done;
      }
      if (!gst_structure_has_field (tmp, "pixel-aspect-ratio"))
        gst_structure_set_value (tmp, "pixel-aspect-ratio", to_par);
      gst_structure_fixate_field_nearest_fraction (tmp, "pixel-aspect-ratio",
          to_par_n, to_par_d);
      gst_structure_get_fraction (tmp, "pixel-aspect-ratio", &set_par_n,
          &set_par_d);
      gst_structure_free (tmp);

      /* Check if the adjusted PAR is accepted */
      if (set_par_n == to_par_n && set_par_d == to_par_d) {
        if (gst_structure_has_field (outs, "pixel-aspect-ratio") ||
            set_par_n != set_par_d)
          gst_structure_set (outs, "height", G_TYPE_INT, set_h,
              "pixel-aspect-ratio", GST_TYPE_FRACTION, set_par_n, set_par_d,
              NULL);
        goto done;
      }

      /* Otherwise scale the height to the new PAR and check if the
       * adjusted with is accepted. If all that fails we can't keep
       * the DAR */
      if (!gst_util_fraction_multiply (from_dar_n, from_dar_d, set_par_d,
              set_par_n, &num, &den)) {
        GST_ELEMENT_ERROR (base, CORE, NEGOTIATION, (NULL),
            ("Error calculating the output scaled size - integer overflow"));
        goto done;
      }

      h = (guint) gst_util_uint64_scale_int (w, den, num);
      gst_structure_fixate_field_nearest_int (outs, "height", h);
      if (gst_structure_has_field (outs, "pixel-aspect-ratio") ||
          set_par_n != set_par_d)
        gst_structure_set (outs, "pixel-aspect-ratio", GST_TYPE_FRACTION,
            set_par_n, set_par_d, NULL);

      goto done;
    } else if (gst_value_is_fixed (to_par)) {
      GstStructure *tmp;
      gint set_h, set_w, f_h, f_w;

      to_par_n = gst_value_get_fraction_numerator (to_par);
      to_par_d = gst_value_get_fraction_denominator (to_par);

      /* Calculate scale factor for the PAR change */
      if (!gst_util_fraction_multiply (from_dar_n, from_dar_d, to_par_n,
              to_par_d, &num, &den)) {
        GST_ELEMENT_ERROR (base, CORE, NEGOTIATION, (NULL),
            ("Error calculating the output scaled size - integer overflow"));
        goto done;
      }

      /* Try to keep the input height (because of interlacing) */
      tmp = gst_structure_copy (outs);
      gst_structure_fixate_field_nearest_int (tmp, "height", from_h);
      gst_structure_get_int (tmp, "height", &set_h);

      /* This might have failed but try to scale the width
       * to keep the DAR nonetheless */
      w = (guint) gst_util_uint64_scale_int (set_h, num, den);
      gst_structure_fixate_field_nearest_int (tmp, "width", w);
      gst_structure_get_int (tmp, "width", &set_w);
      gst_structure_free (tmp);

      /* We kept the DAR and the height is nearest to the original height */
      if (set_w == w) {
        gst_structure_set (outs, "width", G_TYPE_INT, set_w, "height",
            G_TYPE_INT, set_h, NULL);
        goto done;
      }

      f_h = set_h;
      f_w = set_w;

      /* If the former failed, try to keep the input width at least */
      tmp = gst_structure_copy (outs);
      gst_structure_fixate_field_nearest_int (tmp, "width", from_w);
      gst_structure_get_int (tmp, "width", &set_w);

      /* This might have failed but try to scale the width
       * to keep the DAR nonetheless */
      h = (guint) gst_util_uint64_scale_int (set_w, den, num);
      gst_structure_fixate_field_nearest_int (tmp, "height", h);
      gst_structure_get_int (tmp, "height", &set_h);
      gst_structure_free (tmp);

      /* We kept the DAR and the width is nearest to the original width */
      if (set_h == h) {
        gst_structure_set (outs, "width", G_TYPE_INT, set_w, "height",
            G_TYPE_INT, set_h, NULL);
        goto done;
      }

      /* If all this failed, keep the height that was nearest to the orignal
       * height and the nearest possible width. This changes the DAR but
       * there's not much else to do here.
       */
      gst_structure_set (outs, "width", G_TYPE_INT, f_w, "height", G_TYPE_INT,
          f_h, NULL);
      goto done;
    } else {
      GstStructure *tmp;
      gint set_h, set_w, set_par_n, set_par_d, tmp2;

      /* width, height and PAR are not fixed but passthrough is not possible */

      /* First try to keep the height and width as good as possible
       * and scale PAR */
      tmp = gst_structure_copy (outs);
      gst_structure_fixate_field_nearest_int (tmp, "height", from_h);
      gst_structure_get_int (tmp, "height", &set_h);
      gst_structure_fixate_field_nearest_int (tmp, "width", from_w);
      gst_structure_get_int (tmp, "width", &set_w);

      if (!gst_util_fraction_multiply (from_dar_n, from_dar_d, set_h, set_w,
              &to_par_n, &to_par_d)) {
        GST_ELEMENT_ERROR (base, CORE, NEGOTIATION, (NULL),
            ("Error calculating the output scaled size - integer overflow"));
        gst_structure_free (tmp);
        goto done;
      }

      if (!gst_structure_has_field (tmp, "pixel-aspect-ratio"))
        gst_structure_set_value (tmp, "pixel-aspect-ratio", to_par);
      gst_structure_fixate_field_nearest_fraction (tmp, "pixel-aspect-ratio",
          to_par_n, to_par_d);
      gst_structure_get_fraction (tmp, "pixel-aspect-ratio", &set_par_n,
          &set_par_d);
      gst_structure_free (tmp);

      if (set_par_n == to_par_n && set_par_d == to_par_d) {
        gst_structure_set (outs, "width", G_TYPE_INT, set_w, "height",
            G_TYPE_INT, set_h, NULL);

        if (gst_structure_has_field (outs, "pixel-aspect-ratio") ||
            set_par_n != set_par_d)
          gst_structure_set (outs, "pixel-aspect-ratio", GST_TYPE_FRACTION,
              set_par_n, set_par_d, NULL);
        goto done;
      }

      /* Otherwise try to scale width to keep the DAR with the set
       * PAR and height */
      if (!gst_util_fraction_multiply (from_dar_n, from_dar_d, set_par_d,
              set_par_n, &num, &den)) {
        GST_ELEMENT_ERROR (base, CORE, NEGOTIATION, (NULL),
            ("Error calculating the output scaled size - integer overflow"));
        goto done;
      }

      w = (guint) gst_util_uint64_scale_int (set_h, num, den);
      tmp = gst_structure_copy (outs);
      gst_structure_fixate_field_nearest_int (tmp, "width", w);
      gst_structure_get_int (tmp, "width", &tmp2);
      gst_structure_free (tmp);

      if (tmp2 == w) {
        gst_structure_set (outs, "width", G_TYPE_INT, tmp2, "height",
            G_TYPE_INT, set_h, NULL);
        if (gst_structure_has_field (outs, "pixel-aspect-ratio") ||
            set_par_n != set_par_d)
          gst_structure_set (outs, "pixel-aspect-ratio", GST_TYPE_FRACTION,
              set_par_n, set_par_d, NULL);
        goto done;
      }

      /* ... or try the same with the height */
      h = (guint) gst_util_uint64_scale_int (set_w, den, num);
      tmp = gst_structure_copy (outs);
      gst_structure_fixate_field_nearest_int (tmp, "height", h);
      gst_structure_get_int (tmp, "height", &tmp2);
      gst_structure_free (tmp);

      if (tmp2 == h) {
        gst_structure_set (outs, "width", G_TYPE_INT, set_w, "height",
            G_TYPE_INT, tmp2, NULL);
        if (gst_structure_has_field (outs, "pixel-aspect-ratio") ||
            set_par_n != set_par_d)
          gst_structure_set (outs, "pixel-aspect-ratio", GST_TYPE_FRACTION,
              set_par_n, set_par_d, NULL);
        goto done;
      }

      /* If all fails we can't keep the DAR and take the nearest values
       * for everything from the first try */
      gst_structure_set (outs, "width", G_TYPE_INT, set_w, "height",
          G_TYPE_INT, set_h, NULL);
      if (gst_structure_has_field (outs, "pixel-aspect-ratio") ||
          set_par_n != set_par_d)
        gst_structure_set (outs, "pixel-aspect-ratio", GST_TYPE_FRACTION,
            set_par_n, set_par_d, NULL);
    }
  }

done:
  GST_DEBUG_OBJECT (base, "fixated othercaps to %" GST_PTR_FORMAT, othercaps);

  if (from_par == &fpar)
    g_value_unset (&fpar);
  if (to_par == &tpar)
    g_value_unset (&tpar);

  return othercaps;
}

static GstFlowReturn
gst_xlnx_video_scale_transform_frame (GstVideoFilter * filter,
    GstVideoFrame * in_frame, GstVideoFrame * out_frame)
{
  GstXlnxVideoScale *videoscale = GST_XLNX_VIDEO_SCALE_CAST (filter);
  int iret = 1;

  GST_CAT_DEBUG_OBJECT (CAT_PERFORMANCE, videoscale, "doing video transform");

  gst_xlnx_video_scale_time_division_lock (videoscale);

  if (!xlnx_video_scale_configure_vpss (videoscale, in_frame, out_frame)) {
    GST_ERROR_OBJECT (videoscale, "failed to configure VPSS");
    goto error;
  }

  if (!xlnx_video_scale_register_dmabuf (videoscale, out_frame,
          videoscale->fbwr_fd, FALSE)) {
    GST_ERROR_OBJECT (videoscale, "failed to configure FB write IP");
    goto error;
  }

  GST_LOG_OBJECT (videoscale, "registered output dmabuf successfully");

  if (!xlnx_video_scale_register_dmabuf (videoscale, in_frame,
          videoscale->fbrd_fd, TRUE)) {
    GST_ERROR_OBJECT (videoscale, "failed to configure FB read IP");
    goto error;
  }

  GST_LOG_OBJECT (videoscale, "registered input dmabuf successfully");

  /* TODO: Once, driver enables interrupt based model, plugin should add polling mechanism */
  while (iret)
    iret = ioctl (videoscale->fbrd_fd, XSET_FB_POLL, NULL);

  if (!xlnx_video_scale_unregister_dmabuf (videoscale, out_frame,
          videoscale->fbwr_fd)) {
    GST_ERROR_OBJECT (videoscale, "failed to configure FB write IP");
    goto error;
  }

  if (!xlnx_video_scale_unregister_dmabuf (videoscale, in_frame,
          videoscale->fbrd_fd)) {
    GST_ERROR_OBJECT (videoscale, "failed to configure FB read IP");
    goto error;
  }

  gst_xlnx_video_scale_time_division_unlock (videoscale);

  return GST_FLOW_OK;

error:
  gst_xlnx_video_scale_time_division_unlock (videoscale);
  return GST_FLOW_ERROR;
}

static gboolean
plugin_init (GstPlugin * plugin)
{
  if (!gst_element_register (plugin, "xlnxvideoscale", GST_RANK_NONE,
          GST_TYPE_XLNX_VIDEO_SCALE))
    return FALSE;

  GST_DEBUG_CATEGORY_INIT (xlnx_video_scale_debug, "xlnxvideoscale", 0,
      "Xilinx videoscale element");
  GST_DEBUG_CATEGORY_GET (CAT_PERFORMANCE, "GST_PERFORMANCE");

  return TRUE;
}

/* PACKAGE: this is usually set by autotools depending on some _INIT macro
 * in configure.ac and then written into and defined in config.h, but we can
 * just set it ourselves here in case someone doesn't use autotools to
 * compile this code. GST_PLUGIN_DEFINE needs PACKAGE to be defined.
 */
#ifndef PACKAGE
#define PACKAGE "xlnxvideoscale"
#endif

GST_PLUGIN_DEFINE (GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    xlnxvideoscale,
    "Resizes video",
    plugin_init, "0.1", "LGPL", "GStreamer Xilinx", "http://xilinx.com/")
