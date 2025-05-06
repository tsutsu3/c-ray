//
//  c-ray.h
//  libc-ray
//
//  Created by Valtteri on 5.1.2020.
//  Copyright © 2020-2023 Valtteri Koskivuori. All rights reserved.
//

#ifndef C_RAY_H
#define C_RAY_H

#ifdef __cplusplus
extern "C" {
#endif

// c-ray public-facing API

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef CR_BUILDING_LIB
	#if defined(_WIN32) || defined(__CYGWIN__)
		#define CR_EXPORT __declspec(dllexport)
	#elif defined(__GNUC__)
		#define CR_EXPORT __attribute__((visibility("default")))
	#else
		#define CR_EXPORT
	#endif
#else
	#define CR_EXPORT
#endif

#define C_RAY_PROTO_DEFAULT_PORT 2222
#define MAX_CRAY_VERTEX_COUNT 3

// -- Library info --
CR_EXPORT char *cr_get_version(void); //The current semantic version
CR_EXPORT char *cr_get_git_hash(void); //The current git hash of the build

// -- Renderer --
struct cr_renderer;
CR_EXPORT struct cr_renderer *cr_new_renderer(void);
CR_EXPORT void cr_destroy_renderer(struct cr_renderer *r);

enum cr_renderer_param {
	// Num
	cr_renderer_threads = 0,
	cr_renderer_samples,
	cr_renderer_bounces,
	cr_renderer_tile_width,
	cr_renderer_tile_height,
	cr_renderer_tile_order,
	cr_renderer_override_width,
	cr_renderer_override_height,
	cr_renderer_override_cam,
	cr_renderer_is_iterative,
	// String
	cr_renderer_asset_path,
	cr_renderer_node_list,
	cr_renderer_blender_mode,
};

enum cr_tile_state {
	cr_tile_ready_to_render = 0,
	cr_tile_rendering,
	cr_tile_finished
};

struct cr_tile {
	int w;
	int h;
	int start_x;
	int start_y;
	int end_x;
	int end_y;
	enum cr_tile_state state;
	bool network_renderer;
	int index;
	size_t total_samples;
	size_t completed_samples;
};

struct cr_renderer_cb_info {
	const struct cr_bitmap **fb;
	const struct cr_tile *tiles;
	size_t tiles_count;

	size_t active_threads;
	double avg_per_ray_us;
	int64_t samples_per_sec;
	int64_t eta_ms;
	size_t finished_passes;
	double completion;
	bool paused;
	bool aborted;
};

enum cr_renderer_callback {
	cr_cb_on_start = 0,
	cr_cb_on_stop,
	cr_cb_status_update,
	cr_cb_on_state_changed,
	cr_cb_on_interactive_pass_finished,
};

CR_EXPORT bool cr_renderer_set_callback(struct cr_renderer *ext,
                                        enum cr_renderer_callback t,
                                        void (*callback_fn)(struct cr_renderer_cb_info *, void *),
                                        void *user_data);

CR_EXPORT bool cr_renderer_set_num_pref(struct cr_renderer *ext, enum cr_renderer_param p, uint64_t num);
CR_EXPORT bool cr_renderer_set_str_pref(struct cr_renderer *ext, enum cr_renderer_param p, const char *str);
CR_EXPORT void cr_renderer_stop(struct cr_renderer *ext);
CR_EXPORT void cr_renderer_restart_interactive(struct cr_renderer *ext);
CR_EXPORT void cr_renderer_toggle_pause(struct cr_renderer *ext);
CR_EXPORT const char *cr_renderer_get_str_pref(struct cr_renderer *ext, enum cr_renderer_param p);
CR_EXPORT uint64_t cr_renderer_get_num_pref(struct cr_renderer *ext, enum cr_renderer_param p);

struct cr_bitmap {
	enum cr_bm_colorspace {
		cr_bm_linear = 0,
		cr_bm_sRGB
	} colorspace;
	enum cr_bm_channel_precision {
		cr_bm_char,
		cr_bm_float
	} precision;
	union {
		unsigned char *byte_ptr;
		float *float_ptr;
	} data;
	size_t stride;
	size_t width;
	size_t height;
};

CR_EXPORT void cr_renderer_render(struct cr_renderer *r);
CR_EXPORT void cr_renderer_start_interactive(struct cr_renderer *ext);
CR_EXPORT struct cr_bitmap *cr_renderer_get_result(struct cr_renderer *r);

// -- Scene --

struct cr_vector {
	float x, y, z;
};

struct cr_coord {
	float u, v;
};

struct cr_scene;
CR_EXPORT struct cr_scene *cr_renderer_scene_get(struct cr_renderer *r);

struct cr_scene_totals {
	size_t meshes;
	size_t spheres;
	size_t instances;
	size_t cameras;
};
CR_EXPORT struct cr_scene_totals cr_get_scene_totals(struct cr_scene *s_ext);

struct cr_color {
	float r;
	float g;
	float b;
	float a;
};

typedef int64_t cr_object;

typedef cr_object cr_sphere;
CR_EXPORT cr_sphere cr_scene_add_sphere(struct cr_scene *s_ext, float radius);
typedef cr_object cr_mesh;

struct cr_vertex_buf_param {
	struct cr_vector *vertices;
	size_t vertex_count;
	struct cr_vector *normals;
	size_t normal_count;
	struct cr_coord *tex_coords;
	size_t tex_coord_count;
};

struct cr_face {
	int vertex_idx[MAX_CRAY_VERTEX_COUNT];
	int normal_idx[MAX_CRAY_VERTEX_COUNT];
	int texture_idx[MAX_CRAY_VERTEX_COUNT];
	uint16_t mat_idx;
	bool has_normals;
};

CR_EXPORT cr_mesh cr_scene_mesh_new(struct cr_scene *s_ext, const char *name);
CR_EXPORT cr_mesh cr_scene_get_mesh(struct cr_scene *s_ext, const char *name);

CR_EXPORT void cr_mesh_bind_vertex_buf(struct cr_scene *s_ext, cr_mesh mesh, struct cr_vertex_buf_param buf);
CR_EXPORT void cr_mesh_bind_faces(struct cr_scene *s_ext, cr_mesh mesh, struct cr_face *faces, size_t face_count);
CR_EXPORT void cr_mesh_finalize(struct cr_scene *s_ext, cr_mesh mesh);

// -- Camera --
// FIXME: Use cr_vector
// TODO: Support quaternions, or maybe just a mtx4x4?
enum cr_camera_param {
	cr_camera_fov,
	cr_camera_focus_distance,
	cr_camera_fstops,

	cr_camera_pose_x,
	cr_camera_pose_y,
	cr_camera_pose_z,
	cr_camera_pose_roll,
	cr_camera_pose_pitch,
	cr_camera_pose_yaw,

	cr_camera_time,

	cr_camera_res_x,
	cr_camera_res_y,
	cr_camera_blender_coord,
};

typedef cr_object cr_camera;
CR_EXPORT cr_camera cr_camera_new(struct cr_scene *ext);
CR_EXPORT bool cr_camera_set_num_pref(struct cr_scene *ext, cr_camera c, enum cr_camera_param p, double num);
CR_EXPORT double cr_camera_get_num_pref(struct cr_scene *ext, cr_camera c, enum cr_camera_param p);
CR_EXPORT bool cr_camera_update(struct cr_scene *ext, cr_camera c);

// -- Materials --
#include "node.h"

typedef cr_object cr_material_set;
typedef cr_object cr_material;
CR_EXPORT cr_material_set cr_scene_new_material_set(struct cr_scene *s_ext);
CR_EXPORT cr_material cr_material_set_add(struct cr_scene *s_ext, cr_material_set set, struct cr_shader_node *desc);
CR_EXPORT void cr_material_update(struct cr_scene *s_ext, cr_material_set set, cr_material mat, struct cr_shader_node *desc);

// -- Instancing --

typedef int64_t cr_instance;
enum cr_object_type {
	cr_object_mesh = 0,
	cr_object_sphere
};

CR_EXPORT cr_instance cr_instance_new(struct cr_scene *s_ext, cr_object object, enum cr_object_type type);
CR_EXPORT void cr_instance_set_transform(struct cr_scene *s_ext, cr_instance instance, float row_major[4][4]);
CR_EXPORT void cr_instance_transform(struct cr_scene *s_ext, cr_instance instance, float row_major[4][4]);
CR_EXPORT bool cr_instance_bind_material_set(struct cr_scene *s_ext, cr_instance instance, cr_material_set set);

// -- Misc. --
CR_EXPORT bool cr_scene_set_background(struct cr_scene *s_ext, struct cr_shader_node *desc);
CR_EXPORT void cr_start_render_worker(int port, size_t thread_limit);
CR_EXPORT void cr_send_shutdown_to_workers(const char *node_list);
CR_EXPORT bool cr_load_json(struct cr_renderer *r_ext, const char *file_path);

enum cr_log_level {
	Silent = 0,
	Info,
	Debug,
	Spam,
};

CR_EXPORT void cr_log_level_set(enum cr_log_level);
CR_EXPORT enum cr_log_level cr_log_level_get(void);

CR_EXPORT void cr_debug_dump_state(struct cr_renderer *r_ext);
#ifdef __cplusplus
}
#endif

#endif
