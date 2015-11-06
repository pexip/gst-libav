/* GStreamer unit tests for libav video encoders/decoders
 *
 * Copyright (C) 2015 Stian Selnes  <stian@pexip.com>
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

#include <gst/check/check.h>
#include <gst/check/gstharness.h>
#include <gst/video/video.h>

static GstBuffer *
create_video_buffer_from_info (GstHarness * h, gint value,
    GstVideoInfo * info, GstClockTime timestamp, GstClockTime duration)
{
  GstBuffer *buf;
  gsize size;

  size = GST_VIDEO_INFO_SIZE (info);

  buf = gst_harness_create_buffer (h, size);
  gst_buffer_memset (buf, 0, value, size);
  g_assert (buf != NULL);

  gst_buffer_add_video_meta_full (buf,
      GST_VIDEO_FRAME_FLAG_NONE,
      GST_VIDEO_INFO_FORMAT (info),
      GST_VIDEO_INFO_WIDTH (info),
      GST_VIDEO_INFO_HEIGHT (info),
      GST_VIDEO_INFO_N_PLANES (info),
      info->offset,
      info->stride);

  GST_BUFFER_PTS (buf) = timestamp;
  GST_BUFFER_DURATION (buf) = duration;

  return buf;
}

static GstBuffer *
create_video_buffer (GstHarness * h, guint width, guint height, guint n)
{
  GstVideoInfo info;

  gst_video_info_init (&info);
  gst_video_info_set_format (&info, GST_VIDEO_FORMAT_I420, width, height);

  return create_video_buffer_from_info (h, 0, &info, n * GST_SECOND / 30,
      GST_SECOND / 30);
}

static GstCaps *
caps_new_video (gint width, gint height, gint fps_n, gint fps_d,
    gint par_n, gint par_d)
{
  GstVideoInfo info;

  gst_video_info_init (&info);
  gst_video_info_set_format (&info, GST_VIDEO_FORMAT_I420, width, height);
  GST_VIDEO_INFO_FPS_N (&info) = fps_n;
  GST_VIDEO_INFO_FPS_D (&info) = fps_d;
  GST_VIDEO_INFO_PAR_N (&info) = par_n;
  GST_VIDEO_INFO_PAR_D (&info) = par_d;

  return gst_video_info_to_caps (&info);
}

struct {
  const gchar *encoder;
  const gchar *decoder;
} encoder_decoder_pairs[] = {
  {"avenc_h261",  "avdec_h261"},
  {"avenc_h263",  "avdec_h263"},
  {"avenc_h263p", "avdec_h263"},
};

static GstPadQueryFunction _gst_harness_sink_query;

static gboolean
allocation_query_add_video_pool (GstPad * pad, GstObject * parent,
    GstQuery * query)
{
  gboolean res;

  switch (GST_QUERY_TYPE (query)) {
    case GST_QUERY_ALLOCATION:
    {
      GstCaps *caps;
      GstVideoInfo info;
      GstBufferPool *pool;

      gst_query_parse_allocation (query, &caps, NULL);
      g_assert_cmpuint (gst_query_get_n_allocation_params (query), ==, 0);
      g_assert_cmpuint (gst_query_get_n_allocation_pools (query), ==, 0);

      fail_unless (gst_video_info_from_caps (&info, caps));

      /* Direct rendering requires pool that supports videometa and alignment */
      pool = gst_video_buffer_pool_new ();
      fail_unless (gst_buffer_pool_has_option (pool,
              GST_BUFFER_POOL_OPTION_VIDEO_META));
      fail_unless (gst_buffer_pool_has_option (pool,
              GST_BUFFER_POOL_OPTION_VIDEO_ALIGNMENT));

      gst_query_add_allocation_pool (query, pool, info.size, 0, 0);
      gst_object_unref (pool);
      gst_query_add_allocation_meta (query, GST_VIDEO_META_API_TYPE, NULL);
      res = TRUE;
      break;
    }
    default:
      res = _gst_harness_sink_query (pad, parent, query);
      break;
  }

  return res;
}

GST_START_TEST (test_decoder_direct_rendering_make_writable_does_not_memcpy)
{
  const gchar *encoder = encoder_decoder_pairs[__i__].encoder;
  const gchar *decoder = encoder_decoder_pairs[__i__].decoder;
  GstBuffer *buf0, *buf1;
  GstMemory *mem0, *mem1;
  GstHarness *h;
  gchar *ll;

  /* Direct rendering should be enabled by default, no need to set it. */
  ll = g_strdup_printf ("%s ! %s", encoder, decoder);
  h = gst_harness_new_parse (ll);
  g_free (ll);

  _gst_harness_sink_query = GST_PAD_QUERYFUNC (h->sinkpad);
  GST_PAD_QUERYFUNC (h->sinkpad) = allocation_query_add_video_pool;

  /* Get output from decoder */
  gst_harness_set_src_caps (h, caps_new_video (176, 144, 30, 1, 1, 1));
  buf0 = create_video_buffer (h, 176, 144, 0);
  buf0 = gst_harness_push_and_pull (h, buf0);

  /* Verify that memory is locked by one or more, meaning the decoder is has
   * mapped the memory for internal reference buffer and direct rendering is
   * enabled. Needed to verify that we actually test what we want to test.
   * NOTE: This makes assumptions about gstminiobject internals. */
  mem0 = gst_buffer_peek_memory (buf0, 0);
  fail_unless ((GST_MINI_OBJECT (mem0)->lockstate & 0xff00) != 0);

  /* Make writable on the decoded buffer. Should reuse the same GstMemory
   * before and after making the buffer (not memory) writable, meaning no
   * memcpy is done. */
  buf1 = gst_buffer_make_writable (buf0);
  mem1 = gst_buffer_peek_memory (buf1, 0);
  fail_unless_equals_pointer (mem0, mem1);
  gst_buffer_unref (buf1);

  gst_harness_teardown (h);
}
GST_END_TEST;

static Suite *
avviddec_suite (void)
{
  Suite *s = suite_create ("avvid");
  TCase *tc_chain;

  suite_add_tcase (s, (tc_chain = tcase_create ("avviddec")));
  tcase_add_loop_test (tc_chain, test_decoder_direct_rendering_make_writable_does_not_memcpy,
      0, G_N_ELEMENTS (encoder_decoder_pairs));

  return s;
}

GST_CHECK_MAIN (avviddec);
