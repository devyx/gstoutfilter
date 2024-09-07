/******************************************************************************
	Copyright (C) 2016-2024 DistroAV <contact@distroav.org>

	This program is free software; you can redistribute it and/or
	modify it under the terms of the GNU General Public License
	as published by the Free Software Foundation; either version 2
	of the License, or (at your option) any later version.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with this program; if not, see <https://www.gnu.org/licenses/>.
******************************************************************************/

#include <util/platform.h>
#include <util/threading.h>
#include <media-io/video-frame.h>

#include <gst/gst.h>
#include <gst/app/app.h>

#include "plugin-support.h"
#include <obs-frontend-api.h>
#include <obs-module.h>

#define PLUGIN_MIN_QT_VERSION "6.0.0"
#define PLUGIN_MIN_OBS_VERSION "30.0.0"

#define TEXFORMAT GS_BGRA
#define FLT_PROP_NAME "gst_out_filter_pipeline"

typedef struct {
	obs_source_t *obs_source;

	GstElement *pipeline;
	GstElement *appsrc;
	uint8_t *request;
	bool restart_pipeline;

	obs_video_info ovi;
	obs_audio_info oai;

	uint32_t known_width;
	uint32_t known_height;

	gs_texrender_t *texrender;
	gs_stagesurf_t *stagesurface;
	uint8_t *video_data;
	uint32_t video_linesize;

	video_t *video_output;
	bool is_audioonly;

	uint8_t *audio_conv_buffer;
	size_t audio_conv_buffer_size;
} gst_out_filter_t;

const char *gst_out_filter_getname(void *)
{
	return "GST Output filter";
}

void gst_out_filter_update(void *data, obs_data_t *settings);

obs_properties_t *gst_out_filter_getproperties(void *)
{
	obs_log(LOG_INFO, "+gst_out_filter_getproperties(...)");
	obs_properties_t *props = obs_properties_create();
	obs_properties_set_flags(props, OBS_PROPERTIES_DEFER_UPDATE);

	obs_properties_add_text(
		props, FLT_PROP_NAME,
		"Pipeline",
		OBS_TEXT_DEFAULT);

	obs_properties_add_button(
		props, "gst_out_apply",
		"Apply",
		[](obs_properties_t *, obs_property_t *, void *private_data) {
			auto s = (gst_out_filter_t *)private_data;
			auto settings = obs_source_get_settings(s->obs_source);
			gst_out_filter_update(s, settings);
			obs_data_release(settings);
			return true;
		});

	obs_log(LOG_INFO, "-gst_out_filter_getproperties(...)");
	return props;
}

void gst_out_filter_getdefaults(obs_data_t *defaults)
{
	obs_log(LOG_INFO, "+gst_out_filter_getdefaults(...)");
	obs_data_set_default_string(
		defaults, FLT_PROP_NAME,
		"");
	obs_log(LOG_INFO, "-gst_out_filter_getdefaults(...)");
}

void gst_out_filter_raw_video(void *data, video_data *frame)
{
	auto f = (gst_out_filter_t *)data;
	//obs_log(LOG_INFO,"++ filter_raw_data");
	if (!frame || !frame->data[0])
		return;

    
	if (f->appsrc != NULL) {
		GstBuffer *gst_buffer;
		gst_buffer = gst_buffer_new_allocate(NULL, frame->linesize[0]*f->known_height, NULL);
    	gst_buffer_fill(gst_buffer, 0, frame->data[0], frame->linesize[0]*f->known_height);
    	GST_BUFFER_PTS(gst_buffer) = GST_CLOCK_TIME_NONE;

        gst_app_src_push_buffer(GST_APP_SRC(f->appsrc), gst_buffer);
    }
}

void gst_out_filter_offscreen_render(void *data, uint32_t, uint32_t)
{
	auto f = (gst_out_filter_t *)data;

	obs_source_t *target = obs_filter_get_parent(f->obs_source);
	if (!target) {
		return;
	}

	uint32_t width = obs_source_get_base_width(target);
	uint32_t height = obs_source_get_base_height(target);

	gs_texrender_reset(f->texrender);

	if (gs_texrender_begin(f->texrender, width, height)) {
		vec4 background;
		vec4_zero(&background);

		gs_clear(GS_CLEAR_COLOR, &background, 0.0f, 0);
		gs_ortho(0.0f, (float)width, 0.0f, (float)height, -100.0f,
			 100.0f);

		gs_blend_state_push();
		gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);

		obs_source_video_render(target);

		gs_blend_state_pop();
		gs_texrender_end(f->texrender);

		if (f->known_width != width || f->known_height != height || f->restart_pipeline) {

			f->restart_pipeline = false;
			gs_stagesurface_destroy(f->stagesurface);
			f->stagesurface = gs_stagesurface_create(width, height,
								 TEXFORMAT);

			video_output_info vi = {0};
			vi.format = VIDEO_FORMAT_BGRA;
			vi.width = width;
			vi.height = height;
			vi.fps_den = f->ovi.fps_den;
			vi.fps_num = f->ovi.fps_num;
			vi.cache_size = 16;
			vi.colorspace = VIDEO_CS_DEFAULT;
			vi.range = VIDEO_RANGE_DEFAULT;
			vi.name = obs_source_get_name(f->obs_source);

			video_output_close(f->video_output);
			video_output_open(&f->video_output, &vi);
			video_output_connect(f->video_output, nullptr,
					     gst_out_filter_raw_video, f);

			f->known_width = width;
			f->known_height = height;

			// restart pipeline
			gchar *pipe_string = g_strdup_printf(
                "appsrc name=appsrc is-live=true leaky-type=1 do-timestamp=true ! video/x-raw, format=BGRA, width=%d, height=%d, framerate=%d/%d, interlace-mode=progressive ! %s",            
            	    f->known_width,f->known_height,f->ovi.fps_num, f->ovi.fps_den,f->request);
    		obs_log(LOG_INFO,"gst === %s",pipe_string);
    		GError *err = NULL;
    		f->pipeline = gst_parse_launch(pipe_string, &err);

    		g_free(pipe_string);

    		if (err != NULL) {
        		obs_log(LOG_INFO, "Fail pipeline %s", err->message);
				f->pipeline = NULL;
				f->appsrc = NULL;	
			} else {
				f->appsrc = gst_bin_get_by_name(GST_BIN(f->pipeline), "appsrc");
				gst_element_set_state(f->pipeline, GST_STATE_PLAYING);
			}
		}

		video_frame output_frame;
		if (video_output_lock_frame(f->video_output, &output_frame, 1,
					    os_gettime_ns())) {
			if (f->video_data) {
				gs_stagesurface_unmap(f->stagesurface);
				f->video_data = nullptr;
			}

			gs_stage_texture(
				f->stagesurface,
				gs_texrender_get_texture(f->texrender));
			gs_stagesurface_map(f->stagesurface, &f->video_data,
					    &f->video_linesize);

			uint32_t linesize = output_frame.linesize[0];
			for (uint32_t i = 0; i < f->known_height; ++i) {
				uint32_t dst_offset = linesize * i;
				uint32_t src_offset = f->video_linesize * i;
				memcpy(output_frame.data[0] + dst_offset,
				       f->video_data + src_offset, linesize);
			}

			video_output_unlock_frame(f->video_output);
		}
	}
}

void gst_out_filter_update(void *data, obs_data_t *settings)
{
	auto f = (gst_out_filter_t *)data;
	auto obs_source = f->obs_source;
	auto name = obs_source_get_name(obs_source);
	obs_log(LOG_INFO, "+gst_out_filter_update(name=`%s`)", name);

	obs_remove_main_render_callback(gst_out_filter_offscreen_render, f);
	auto groups = obs_data_get_string(settings, FLT_PROP_NAME);
	obs_log(LOG_INFO, "+gst_out_filter_update(name=`%s`)", groups);
	f->request = (uint8_t*)groups;

	if (f->pipeline != NULL) {
		// stop and destroy old pipeline
        gst_element_set_state (f->pipeline, GST_STATE_NULL);
        gst_object_unref (f->pipeline);
        f->pipeline = NULL;
        f->appsrc = NULL;
	}

	gchar *pipe_string = g_strdup_printf(
                "appsrc name=appsrc is-live=true leaky-type=1 do-timestamp=true ! video/x-raw, format=BGRA, width=%d, height=%d, framerate=%d/%d, interlace-mode=progressive ! %s",            
                f->known_width,f->known_height,f->ovi.fps_num, f->ovi.fps_den,f->request);
    obs_log(LOG_INFO,"gst = %s",pipe_string);
    GError *err = NULL;
    f->pipeline = gst_parse_launch(pipe_string, &err);

    g_free(pipe_string);
	f->restart_pipeline = true;	
	// check 
    if (err != NULL) {
        obs_log(LOG_INFO, "Fail pipeline %s", err->message);
		f->pipeline = NULL;
		f->appsrc = NULL;
		//return;
	} else {
    	gst_element_set_state (f->pipeline, GST_STATE_NULL);
    	gst_object_unref (f->pipeline);
		f->pipeline = NULL;
    	f->appsrc = NULL;
	}
	
	obs_add_main_render_callback(gst_out_filter_offscreen_render, f);

	obs_log(LOG_INFO, "-gst_out_filter_update(name=`%s`)", name);
}

void *gst_out_filter_create(obs_data_t *settings, obs_source_t *obs_source)
{
	auto name = obs_data_get_string(settings, FLT_PROP_NAME);
	obs_log(LOG_INFO, "+gst_out_filter_create(name=`%s`)", name);

	auto f = (gst_out_filter_t *)bzalloc(sizeof(gst_out_filter_t));
	f->obs_source = obs_source;
	f->pipeline = NULL;
	f->appsrc = NULL;
	f->texrender = gs_texrender_create(TEXFORMAT, GS_ZS_NONE);
	obs_get_video_info(&f->ovi);
	obs_get_audio_info(&f->oai);

	gst_out_filter_update(f, settings);

	obs_log(LOG_INFO, "-gst_out_filter_create(...)");

	return f;
}



void gst_out_filter_destroy(void *data)
{
	auto f = (gst_out_filter_t *)data;
	auto name = obs_source_get_name(f->obs_source);
	obs_log(LOG_INFO, "+gst_out_filter_destroy('%s'...)", name);

	obs_remove_main_render_callback(gst_out_filter_offscreen_render, f);
	video_output_close(f->video_output);

	if (f->pipeline != NULL) {
		// stop and destroy old pipeline
        gst_element_set_state (f->pipeline, GST_STATE_NULL);
        gst_object_unref (f->pipeline);
        f->pipeline = NULL;
        f->appsrc = NULL;
	}	

	gs_stagesurface_unmap(f->stagesurface);
	gs_stagesurface_destroy(f->stagesurface);
	gs_texrender_destroy(f->texrender);

	if (f->audio_conv_buffer) {
		obs_log(LOG_INFO, "gst_out_filter_destroy: freeing %zu bytes",
			f->audio_conv_buffer_size);
		bfree(f->audio_conv_buffer);
		f->audio_conv_buffer = nullptr;
	}

	bfree(f);

	obs_log(LOG_INFO, "-gst_out_filter_destroy('%s'...)", name);
}

void gst_out_filter_tick(void *data, float)
{
	auto f = (gst_out_filter_t *)data;
	obs_get_video_info(&f->ovi);
}

void gst_out_filter_videorender(void *data, gs_effect_t *)
{
	auto f = (gst_out_filter_t *)data;
	obs_source_skip_video_filter(f->obs_source);
}

obs_audio_data *gst_out_filter_asyncaudio(void *data, obs_audio_data *audio_data)
{
	auto f = (gst_out_filter_t *)data;

	obs_get_audio_info(&f->oai);

	return audio_data;
}

obs_source_info create_gst_out_filter_info()
{
	obs_source_info gst_out_filter_info = {};
	gst_out_filter_info.id = "gst_out_filter";
	gst_out_filter_info.type = OBS_SOURCE_TYPE_FILTER;
	gst_out_filter_info.output_flags = OBS_SOURCE_VIDEO;

	gst_out_filter_info.get_name = gst_out_filter_getname;
	gst_out_filter_info.get_properties = gst_out_filter_getproperties;
	gst_out_filter_info.get_defaults = gst_out_filter_getdefaults;

	gst_out_filter_info.create = gst_out_filter_create;
	gst_out_filter_info.destroy = gst_out_filter_destroy;
	gst_out_filter_info.update = gst_out_filter_update;

	gst_out_filter_info.video_tick = gst_out_filter_tick;
	gst_out_filter_info.video_render = gst_out_filter_videorender;

	// Audio is available only with async sources
	gst_out_filter_info.filter_audio = gst_out_filter_asyncaudio;

	return gst_out_filter_info;
}

OBS_DECLARE_MODULE()

const char *obs_module_name()
{
	return PLUGIN_NAME;
}

const char *obs_module_description()
{
	return "Description"; 
}

extern struct obs_source_info create_gst_out_filter_info();
struct obs_source_info gst_out_filter_info;


bool obs_module_load(void)
{
	gst_out_filter_info = create_gst_out_filter_info();
	obs_register_source(&gst_out_filter_info);
	gst_init(NULL, NULL);
	return true;
}

void obs_module_post_load(void)
{
	obs_log(LOG_INFO, "+obs_module_post_load()");

	obs_log(LOG_INFO, "-obs_module_post_load()");
}

void obs_module_unload(void)
{
	obs_log(LOG_INFO, "+obs_module_unload()");
	//gst_deinit();

	obs_log(LOG_INFO, "-obs_module_unload(): goodbye!");
}
