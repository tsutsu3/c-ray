//
//  c-ray.c
//  c-ray
//
//  Created by Valtteri on 5.1.2020.
//  Copyright © 2020-2025 Valtteri Koskivuori. All rights reserved.
//

#include <c-ray/c-ray.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdbool.h>

#include <common/timer.h>
#include <common/gitsha1.h>
#include <common/fileio.h>
#include <common/cr_assert.h>
#include <common/texture.h>
#include <common/cr_string.h>
#include <common/hashtable.h>
#include <common/json_loader.h>
#include <common/node_parse.h>
#include <common/platform/thread_pool.h>
#include <common/platform/signal.h>
#include <accelerators/bvh.h>
#include <renderer/renderer.h>
#include <datatypes/camera.h>
#include <datatypes/scene.h>
#include <protocol/server.h>
#include <protocol/worker.h>
#include <protocol/protocol.h>

#ifdef CRAY_DEBUG_ENABLED
#define DEBUG "D"
#else
#define DEBUG ""
#endif

#define VERSION "0.6.3"DEBUG

char *cr_get_version(void) {
	return VERSION;
}

char *cr_get_git_hash(void) {
	return gitHash();
}

struct cr_shader_node *shader_deepcopy(const struct cr_shader_node *in);

// -- Renderer --

struct cr_renderer;

struct cr_renderer *cr_new_renderer(void) {
	return (struct cr_renderer *)renderer_new();
}

bool cr_renderer_set_callback(struct cr_renderer *ext,
							enum cr_renderer_callback t,
							void (*callback_fn)(struct cr_renderer_cb_info *, void *),
							void *user_data) {
	if (!ext) return false;
	if (t > cr_cb_on_interactive_pass_finished) return false;
	struct renderer *r = (struct renderer *)ext;
	r->state.callbacks[t].fn = callback_fn;
	r->state.callbacks[t].user_data = user_data;
	return true;
}

bool cr_renderer_set_num_pref(struct cr_renderer *ext, enum cr_renderer_param p, uint64_t num) {
	if (!ext) return false;
	struct renderer *r = (struct renderer *)ext;
	switch (p) {
		case cr_renderer_threads: {
			r->prefs.threads = num;
			return true;
		}
		case cr_renderer_samples: {
			r->prefs.sampleCount = num;
			return true;
		}
		case cr_renderer_bounces: {
			if (num > 512) return false;
			r->prefs.bounces = num;
			return true;
		}
		case cr_renderer_tile_width: {
			r->prefs.tileWidth = num;
			return true;
		}
		case cr_renderer_tile_height: {
			r->prefs.tileHeight = num;
			return true;
		}
		case cr_renderer_override_width: {
			r->prefs.override_width = num;
			return true;
		}
		case cr_renderer_override_height: {
			r->prefs.override_height = num;
			return true;
		}
		case cr_renderer_override_cam: {
			if (!r->scene->cameras.count) return false;
			if (num >= r->scene->cameras.count) return false;
			r->prefs.selected_camera = num;
			return true;
		}
		case cr_renderer_is_iterative: {
			r->prefs.iterative = true;
			return true;
		}
		case cr_renderer_blender_mode: {
			r->prefs.blender_mode = num;
			return true;
		}
		default: return false;
	}
	return false;
}

bool cr_renderer_set_str_pref(struct cr_renderer *ext, enum cr_renderer_param p, const char *str) {
	if (!ext) return false;
	struct renderer *r = (struct renderer *)ext;
	switch (p) {
		case cr_renderer_tile_order: {
			if (stringEquals(str, "random")) {
				r->prefs.tileOrder = ro_random;
			} else if (stringEquals(str, "topToBottom")) {
				r->prefs.tileOrder = ro_top_to_bottom;
			} else if (stringEquals(str, "fromMiddle")) {
				r->prefs.tileOrder = ro_from_middle;
			} else if (stringEquals(str, "toMiddle")) {
				r->prefs.tileOrder = ro_to_middle;
			} else {
				r->prefs.tileOrder = ro_normal;
			}
			return true;
		}
		case cr_renderer_asset_path: {
			// TODO: we shouldn't really be touching anything but prefs in here.
			if (r->scene->asset_path) free(r->scene->asset_path);
			r->scene->asset_path = stringCopy(str);
			return true;
		}
		case cr_renderer_node_list: {
			if (r->prefs.node_list) free(r->prefs.node_list);
			r->prefs.node_list = stringCopy(str);
			return true;
		}
		default: return false;
	}
	return false;
}

void cr_renderer_stop(struct cr_renderer *ext) {
	if (!ext) return;
	struct renderer *r = (struct renderer *)ext;
	r->state.s = r_exiting;
	// TODO: use pthread_cond instead of silly busy-waiting loops like this
	do {
		timer_sleep_ms(10);
	} while (r->prefs.iterative && r->state.s == r_exiting);
}

void cr_renderer_toggle_pause(struct cr_renderer *ext) {
	if (!ext) return;
	struct renderer *r = (struct renderer *)ext;
	for (size_t i = 0; i < r->prefs.threads; ++i) {
		// FIXME: Use array for workers
		// FIXME: What about network renderers?
		r->state.workers.items[i].paused = !r->state.workers.items[i].paused;
	}
}

const char *cr_renderer_get_str_pref(struct cr_renderer *ext, enum cr_renderer_param p) {
	if (!ext) return NULL;
	struct renderer *r = (struct renderer *)ext;
	switch (p) {
		case cr_renderer_asset_path: return r->scene->asset_path;
		default: return NULL;
	}
	return NULL;
}

uint64_t cr_renderer_get_num_pref(struct cr_renderer *ext, enum cr_renderer_param p) {
	if (!ext) return 0;
	struct renderer *r = (struct renderer *)ext;
	switch (p) {
		case cr_renderer_threads: return r->prefs.threads;
		case cr_renderer_samples: return r->prefs.sampleCount;
		case cr_renderer_bounces: return r->prefs.bounces;
		case cr_renderer_tile_width: return r->prefs.tileWidth;
		case cr_renderer_tile_height: return r->prefs.tileHeight;
		case cr_renderer_override_width: return r->prefs.override_width;
		case cr_renderer_override_height: return r->prefs.override_height;
		default: return 0; // TODO
	}
	return 0;
}

bool cr_scene_set_background(struct cr_scene *s_ext, struct cr_shader_node *desc) {
	if (!s_ext) return false;
	struct world *s = (struct world *)s_ext;
	s->background = desc ? build_bsdf_node(s_ext, desc) : newBackground(&s->storage, NULL, NULL, NULL, s->use_blender_coordinates);
	if (s->bg_desc) cr_shader_node_free(s->bg_desc);
	s->bg_desc = desc ? shader_deepcopy(desc) : NULL;
	return true;
}

// -- Scene --

struct cr_scene;

struct cr_scene *cr_renderer_scene_get(struct cr_renderer *ext) {
	if (!ext) return NULL;
	struct renderer *r = (struct renderer *)ext;
	struct world *scene = r->scene;
	if (r->prefs.blender_mode) scene->use_blender_coordinates = true;
	return (struct cr_scene *)scene;
}

struct cr_scene_totals cr_get_scene_totals(struct cr_scene *s_ext) {
	if (!s_ext) return (struct cr_scene_totals){ 0 };
	struct world *s = (struct world *)s_ext;
	return (struct cr_scene_totals){
		.meshes = s->meshes.count,
		.spheres = s->spheres.count,
		.instances = s->instances.count,
		.cameras = s->cameras.count
	};
}

cr_sphere cr_scene_add_sphere(struct cr_scene *s_ext, float radius) {
	if (!s_ext) return -1;
	struct world *scene = (struct world *)s_ext;
	return sphere_arr_add(&scene->spheres, (struct sphere){ .radius = radius });
}

struct bvh_build_task_arg {
	struct mesh mesh;
	struct world *scene;
	size_t mesh_idx;
};

void bvh_build_task(void *arg) {
	block_signals();
	// Mesh array may get realloc'd at any time, so we use a copy of mesh while working
	struct bvh_build_task_arg *bt = (struct bvh_build_task_arg *)arg;
	struct timeval timer = { 0 };
	timer_start(&timer);
	struct bvh *bvh = build_mesh_bvh(&bt->mesh);
	long ms = timer_get_ms(timer);
	if (!bvh) {
		logr(debug, "BVH build FAILED for %s\n", bt->mesh.name);
		free(bt);
		return;
	}
	//!//!//!//!//!//!//!//!//!//!//!//!
	thread_rwlock_wrlock(&bt->scene->bvh_lock);
	struct bvh *old_bvh = bt->scene->meshes.items[bt->mesh_idx].bvh;
	bt->scene->meshes.items[bt->mesh_idx].bvh = bvh;
	thread_rwlock_unlock(&bt->scene->bvh_lock);
	//!//!//!//!//!//!//!//!//!//!//!//!
	logr(debug, "BVH %s for %s (%lums)\n", old_bvh ? "updated" : "built", bt->mesh.name, ms);
	destroy_bvh(old_bvh);
	free(bt);
}

void cr_mesh_bind_vertex_buf(struct cr_scene *s_ext, cr_mesh mesh, struct cr_vertex_buf_param buf) {
	if (!s_ext) return;
	struct world *scene = (struct world *)s_ext;
	if ((size_t)mesh > scene->meshes.count - 1) return;
	struct mesh *m = &scene->meshes.items[mesh];
	struct vertex_buffer new = { 0 };
	// TODO: T_arr_add_n()
	if (buf.vertices && buf.vertex_count) {
		for (size_t i = 0; i < buf.vertex_count; ++i) {
			vector_arr_add(&new.vertices, *(struct vector *)&buf.vertices[i]);
		}
	}
	if (buf.normals && buf.normal_count) {
		for (size_t i = 0; i < buf.normal_count; ++i) {
			vector_arr_add(&new.normals, *(struct vector *)&buf.normals[i]);
		}
	}
	if (buf.tex_coords && buf.tex_coord_count) {
		for (size_t i = 0; i < buf.tex_coord_count; ++i) {
			coord_arr_add(&new.texture_coords, *(struct coord *)&buf.tex_coords[i]);
		}
	}
	m->vbuf = new;
}

void cr_mesh_bind_faces(struct cr_scene *s_ext, cr_mesh mesh, struct cr_face *faces, size_t face_count) {
	if (!s_ext || !faces) return;
	struct world *scene = (struct world *)s_ext;
	if ((size_t)mesh > scene->meshes.count - 1) return;
	struct mesh *m = &scene->meshes.items[mesh];
	// FIXME: memcpy
	for (size_t i = 0; i < face_count; ++i) {
		poly_arr_add(&m->polygons, *(struct poly *)&faces[i]);
	}
}

void cr_mesh_finalize(struct cr_scene *s_ext, cr_mesh mesh) {
	if (!s_ext) return;
	struct world *scene = (struct world *)s_ext;
	if ((size_t)mesh > scene->meshes.count - 1) return;
	struct mesh *m = &scene->meshes.items[mesh];
	struct bvh_build_task_arg *arg = calloc(1, sizeof(*arg));
	arg->mesh = *m;
	arg->scene = scene;
	arg->mesh_idx = mesh;
	thread_pool_enqueue(scene->bg_worker, bvh_build_task, arg);
}

cr_mesh cr_scene_mesh_new(struct cr_scene *s_ext, const char *name) {
	if (!s_ext) return -1;
	struct world *scene = (struct world *)s_ext;
	struct mesh new = { 0 };
	if (name) new.name = stringCopy(name);
	thread_rwlock_wrlock(&scene->bvh_lock);
	cr_mesh idx = mesh_arr_add(&scene->meshes, new);
	thread_rwlock_unlock(&scene->bvh_lock);
	return idx;
}

cr_mesh cr_scene_get_mesh(struct cr_scene *s_ext, const char *name) {
	if (!s_ext || !name) return -1;
	struct world *scene = (struct world *)s_ext;
	for (size_t i = 0; i < scene->meshes.count; ++i) {
		if (stringEquals(scene->meshes.items[i].name, name)) {
			return i;
		}
	}
	return -1;
}

cr_instance cr_instance_new(struct cr_scene *s_ext, cr_object object, enum cr_object_type type) {
	if (!s_ext) return -1;
	struct world *scene = (struct world *)s_ext;
	struct instance new;
	switch (type) {
		case cr_object_mesh:
			new = new_mesh_instance(&scene->meshes, object, NULL, NULL);
			break;
		case cr_object_sphere:
			new = new_sphere_instance(&scene->spheres, object, NULL, NULL);
			break;
		default:
			return -1;
	}
	scene->top_level_dirty = true;
	return instance_arr_add(&scene->instances, new);
}

static inline struct matrix4x4 mtx_convert(float row_major[4][4]) {
	return (struct matrix4x4){
		.mtx = {
			{ row_major[0][0], row_major[0][1], row_major[0][2], row_major[0][3] },
			{ row_major[1][0], row_major[1][1], row_major[1][2], row_major[1][3] },
			{ row_major[2][0], row_major[2][1], row_major[2][2], row_major[2][3] },
			{ row_major[3][0], row_major[3][1], row_major[3][2], row_major[3][3] },
		}
	};
}

void cr_instance_set_transform(struct cr_scene *s_ext, cr_instance instance, float row_major[4][4]) {
	if (!s_ext) return;
	struct world *scene = (struct world *)s_ext;
	if ((size_t)instance > scene->instances.count - 1) return;
	struct instance *i = &scene->instances.items[instance];
	struct matrix4x4 mtx = mtx_convert(row_major);
	if (memcmp(&i->composite, &mtx, sizeof(mtx)) == 0) return;
	i->composite = (struct transform){
		.A = mtx,
		.Ainv = mat_invert(mtx)
	};
	scene->top_level_dirty = true;
}

void cr_instance_transform(struct cr_scene *s_ext, cr_instance instance, float row_major[4][4]) {
	if (!s_ext) return;
	struct world *scene = (struct world *)s_ext;
	if ((size_t)instance > scene->instances.count - 1) return;
	struct instance *i = &scene->instances.items[instance];
	struct matrix4x4 mtx = mtx_convert(row_major);
	i->composite.A = mat_mul(i->composite.A, mtx);
	i->composite.Ainv = mat_invert(i->composite.A);
	scene->top_level_dirty = true;
}

bool cr_instance_bind_material_set(struct cr_scene *s_ext, cr_instance instance, cr_material_set set) {
	if (!s_ext) return false;
	struct world *scene = (struct world *)s_ext;
	if ((size_t)instance > scene->instances.count - 1) return false;
	if ((size_t)set > scene->shader_buffers.count - 1) return false;
	struct instance *i = &scene->instances.items[instance];
	i->bbuf_idx = set;
	return true;
}

void cr_destroy_renderer(struct cr_renderer *ext) {
	struct renderer *r = (struct renderer *)ext;
	ASSERT(r);
	renderer_destroy(r);
}

// -- Camera --

struct camera default_camera = {
	.FOV = 80.0f,
	.focus_distance = 0.0f,
	.fstops = 0.0f,
	.width = 800,
	.height = 600,
	.look_at = { 0.0f, 0.0f, 1.0f },
	.forward = { 0.0f, 0.0f, 1.0f },
	.right = { 1.0f, 0.0f, 0.0f },
	.up = { 0.0f, 1.0f, 0.0f },
	.is_blender = false,
};

cr_camera cr_camera_new(struct cr_scene *ext) {
	if (!ext) return -1;
	struct world *scene = (struct world *)ext;
	return camera_arr_add(&scene->cameras, default_camera);
}

bool cr_camera_set_num_pref(struct cr_scene *ext, cr_camera c, enum cr_camera_param p, double num) {
	if (c < 0 || !ext) return false;
	struct world *scene = (struct world *)ext;
	if ((size_t)c > scene->cameras.count - 1) return false;
	struct camera *cam = &scene->cameras.items[c];
	switch (p) {
		case cr_camera_fov: {
			cam->FOV = num;
			return true;
		}
		case cr_camera_focus_distance: {
			cam->focus_distance = num;
			return true;
		}
		case cr_camera_fstops: {
			cam->fstops = num;
			return true;
		}
		case cr_camera_pose_x: {
			cam->position.x = num;
			return true;
		}
		case cr_camera_pose_y: {
			cam->position.y = num;
			return true;
		}
		case cr_camera_pose_z: {
			cam->position.z = num;
			return true;
		}
		case cr_camera_pose_roll: {
			cam->orientation.roll = num;
			return true;
		}
		case cr_camera_pose_pitch: {
			cam->orientation.pitch = num;
			return true;
		}
		case cr_camera_pose_yaw: {
			cam->orientation.yaw = num;
			return true;
		}
		case cr_camera_time: {
			cam->time = num;
			return true;
		}
		case cr_camera_res_x: {
			cam->width = num;
			return true;
		}
		case cr_camera_res_y: {
			cam->height = num;
			return true;
		}
		case cr_camera_blender_coord: {
			cam->look_at = (struct vector){0.0f, 0.0f, -1.0f};
			cam->forward = vec_normalize(cam->look_at);
			cam->right = (struct vector){1.0f, 0.0f, 0.0f};
			cam->up = (struct vector){0.0f, -1.0f, 0.0f};
			cam->is_blender = true;
			return true;
		}
	}

	cam_update_pose(cam, &cam->orientation, &cam->position);
	return false;
}

double cr_camera_get_num_pref(struct cr_scene *ext, cr_camera c, enum cr_camera_param p) {
	if (c < 0 || !ext) return 0.0;
	struct world *scene = (struct world *)ext;
	if ((size_t)c > scene->cameras.count - 1) return 0.0;
	struct camera *cam = &scene->cameras.items[c];
	switch (p) {
		case cr_camera_fov: {
			return cam->FOV;
		}
		case cr_camera_focus_distance: {
			return cam->focus_distance;
		}
		case cr_camera_fstops: {
			return cam->fstops;
		}
		case cr_camera_pose_x: {
			return cam->position.x;
		}
		case cr_camera_pose_y: {
			return cam->position.y;
		}
		case cr_camera_pose_z: {
			return cam->position.z;
		}
		case cr_camera_pose_roll: {
			return cam->orientation.roll;
		}
		case cr_camera_pose_pitch: {
			return cam->orientation.pitch;
		}
		case cr_camera_pose_yaw: {
			return cam->orientation.yaw;
		}
		case cr_camera_time: {
			return cam->time;
		}
		case cr_camera_res_x: {
			return cam->width;
		}
		case cr_camera_res_y: {
			return cam->height;
		}
		case cr_camera_blender_coord: {
			return cam->is_blender ? 1.0 : 0.0;
		}
	}
	return 0.0;
}

bool cr_camera_update(struct cr_scene *ext, cr_camera c) {
	if (c < 0 || !ext) return false;
	struct world *scene = (struct world *)ext;
	if ((size_t)c > scene->cameras.count - 1) return false;
	struct camera *cam = &scene->cameras.items[c];
	cam_update_pose(cam, &cam->orientation, &cam->position);
	cam_recompute_optics(cam);
	return true;
}

bool cr_camera_remove(struct cr_scene *s, cr_camera c) {
	//TODO
	(void)s;
	(void)c;
	return false;
}

// --

cr_material_set cr_scene_new_material_set(struct cr_scene *s_ext) {
	if (!s_ext) return -1;
	struct world *scene = (struct world *)s_ext;
	return bsdf_buffer_arr_add(&scene->shader_buffers, (struct bsdf_buffer){ 0 });
}

struct cr_vector_node *vector_deepcopy(const struct cr_vector_node *in);
struct cr_color_node *color_deepcopy(const struct cr_color_node *in);

struct cr_value_node *value_deepcopy(const struct cr_value_node *in) {
	if (!in) return NULL;
	struct cr_value_node *out = calloc(1, sizeof(*out));
	out->type = in->type;
	switch (in->type) {
		case cr_vn_constant:
			out->arg.constant = in->arg.constant;
			break;
		case cr_vn_fresnel:
			out->arg.fresnel.IOR = value_deepcopy(in->arg.fresnel.IOR);
			out->arg.fresnel.normal = vector_deepcopy(in->arg.fresnel.normal);
			break;
		case cr_vn_map_range:
			out->arg.map_range.input_value = value_deepcopy(in->arg.map_range.input_value);
			out->arg.map_range.from_max = value_deepcopy(in->arg.map_range.from_max);
			out->arg.map_range.from_min = value_deepcopy(in->arg.map_range.from_min);
			out->arg.map_range.to_max = value_deepcopy(in->arg.map_range.to_max);
			out->arg.map_range.to_min = value_deepcopy(in->arg.map_range.to_min);
			break;
		case cr_vn_light_path:
			out->arg.light_path.query = in->arg.light_path.query;
			break;
		case cr_vn_alpha:
			out->arg.alpha.color = color_deepcopy(in->arg.alpha.color);
			break;
		case cr_vn_vec_to_value:
			out->arg.vec_to_value.comp = in->arg.vec_to_value.comp;
			out->arg.vec_to_value.vec = vector_deepcopy(in->arg.vec_to_value.vec);
			break;
		case cr_vn_math:
			out->arg.math.A = value_deepcopy(in->arg.math.A);
			out->arg.math.B = value_deepcopy(in->arg.math.B);
			out->arg.math.op = in->arg.math.op;
			break;
		case cr_vn_grayscale:
			out->arg.grayscale.color = color_deepcopy(in->arg.grayscale.color);
			break;
		default:
			break;

	}
	return out;
}

struct cr_color_node *color_deepcopy(const struct cr_color_node *in) {
	if (!in) return NULL;
	struct cr_color_node *out = calloc(1, sizeof(*out));
	out->type = in->type;
	switch (in->type) {
		case cr_cn_constant:
			out->arg.constant = in->arg.constant;
			break;
		case cr_cn_image:
			out->arg.image.full_path = stringCopy(in->arg.image.full_path);
			out->arg.image.options = in->arg.image.options;
			break;
		case cr_cn_checkerboard:
			out->arg.checkerboard.a = color_deepcopy(in->arg.checkerboard.a);
			out->arg.checkerboard.b = color_deepcopy(in->arg.checkerboard.b);
			out->arg.checkerboard.scale = value_deepcopy(in->arg.checkerboard.scale);
			break;
		case cr_cn_blackbody:
			out->arg.blackbody.degrees = value_deepcopy(in->arg.blackbody.degrees);
			break;
		case cr_cn_split:
			out->arg.split.node = value_deepcopy(in->arg.split.node);
			break;
		case cr_cn_rgb:
			out->arg.rgb.red = value_deepcopy(in->arg.rgb.red);
			out->arg.rgb.green = value_deepcopy(in->arg.rgb.green);
			out->arg.rgb.blue = value_deepcopy(in->arg.rgb.blue);
			break;
		case cr_cn_hsl:
			out->arg.hsl.H = value_deepcopy(in->arg.hsl.H);
			out->arg.hsl.S = value_deepcopy(in->arg.hsl.S);
			out->arg.hsl.L = value_deepcopy(in->arg.hsl.L);
			break;
		case cr_cn_hsv:
			out->arg.hsv.H = value_deepcopy(in->arg.hsv.H);
			out->arg.hsv.S = value_deepcopy(in->arg.hsv.S);
			out->arg.hsv.V = value_deepcopy(in->arg.hsv.V);
			break;
		case cr_cn_hsv_tform:
			out->arg.hsv_tform.tex = color_deepcopy(in->arg.hsv_tform.tex);
			out->arg.hsv_tform.H = value_deepcopy(in->arg.hsv_tform.H);
			out->arg.hsv_tform.S = value_deepcopy(in->arg.hsv_tform.S);
			out->arg.hsv_tform.V = value_deepcopy(in->arg.hsv_tform.V);
			out->arg.hsv_tform.f = value_deepcopy(in->arg.hsv_tform.f);
			break;
		case cr_cn_vec_to_color:
			out->arg.vec_to_color.vec = vector_deepcopy(in->arg.vec_to_color.vec);
			break;
		case cr_cn_gradient:
			out->arg.gradient.a = color_deepcopy(in->arg.gradient.a);
			out->arg.gradient.b = color_deepcopy(in->arg.gradient.b);
			break;
		case cr_cn_color_mix:
			out->arg.color_mix.a = color_deepcopy(in->arg.color_mix.a);
			out->arg.color_mix.b = color_deepcopy(in->arg.color_mix.b);
			out->arg.color_mix.factor = value_deepcopy(in->arg.color_mix.factor);
			break;
		case cr_cn_color_ramp:
			out->arg.color_ramp.factor = value_deepcopy(in->arg.color_ramp.factor);
			out->arg.color_ramp.color_mode = in->arg.color_ramp.color_mode;
			out->arg.color_ramp.interpolation = in->arg.color_ramp.interpolation;
			out->arg.color_ramp.element_count = in->arg.color_ramp.element_count;
			int ct = out->arg.color_ramp.element_count;
			out->arg.color_ramp.elements = calloc(ct, sizeof(*out->arg.color_ramp.elements));
			for (int i = 0; i < ct; ++i) out->arg.color_ramp.elements[i] = in->arg.color_ramp.elements[i];
			break;
		default: // FIXME: default remove
			break;
	}
	return out;
}

struct cr_vector_node *vector_deepcopy(const struct cr_vector_node *in) {
	if (!in) return NULL;
	struct cr_vector_node *out = calloc(1, sizeof(*out));
	out->type = in->type;
	switch (in->type) {
		case cr_vec_constant:
			out->arg.constant = in->arg.constant;
		case cr_vec_normal:
		case cr_vec_uv:
			break;
		case cr_vec_vecmath:
			out->arg.vecmath.A = vector_deepcopy(in->arg.vecmath.A);
			out->arg.vecmath.B = vector_deepcopy(in->arg.vecmath.B);
			out->arg.vecmath.C = vector_deepcopy(in->arg.vecmath.C);
			out->arg.vecmath.f = value_deepcopy(in->arg.vecmath.f);
			out->arg.vecmath.op = in->arg.vecmath.op;
			break;
		case cr_vec_mix:
			out->arg.vec_mix.A = vector_deepcopy(in->arg.vec_mix.A);
			out->arg.vec_mix.B = vector_deepcopy(in->arg.vec_mix.B);
			out->arg.vec_mix.factor = value_deepcopy(in->arg.vec_mix.factor);
			break;
		default:
			break;
	}
	return out;
}

struct cr_shader_node *shader_deepcopy(const struct cr_shader_node *in) {
	if (!in) return NULL;
	struct cr_shader_node *out = calloc(1, sizeof(*out));
	out->type = in->type;
	switch (in->type) {
		case cr_bsdf_diffuse:
			out->arg.diffuse.color = color_deepcopy(in->arg.diffuse.color);
			break;
		case cr_bsdf_metal:
			out->arg.metal.color = color_deepcopy(in->arg.metal.color);
			out->arg.metal.roughness = value_deepcopy(in->arg.metal.roughness);
			break;
		case cr_bsdf_glass:
			out->arg.glass.color = color_deepcopy(in->arg.glass.color);
			out->arg.glass.roughness = value_deepcopy(in->arg.glass.roughness);
			out->arg.glass.IOR = value_deepcopy(in->arg.glass.IOR);
			break;
		case cr_bsdf_plastic:
			out->arg.plastic.color = color_deepcopy(in->arg.plastic.color);
			out->arg.plastic.roughness = value_deepcopy(in->arg.plastic.roughness);
			out->arg.plastic.IOR = value_deepcopy(in->arg.plastic.IOR);
			break;
		case cr_bsdf_mix:
			out->arg.mix.A = shader_deepcopy(in->arg.mix.A);
			out->arg.mix.B = shader_deepcopy(in->arg.mix.B);
			out->arg.mix.factor = value_deepcopy(in->arg.mix.factor);
			break;
		case cr_bsdf_add:
			out->arg.add.A = shader_deepcopy(in->arg.add.A);
			out->arg.add.B = shader_deepcopy(in->arg.add.B);
			break;
		case cr_bsdf_transparent:
			out->arg.transparent.color = color_deepcopy(in->arg.transparent.color);
			break;
		case cr_bsdf_emissive:
			out->arg.emissive.color = color_deepcopy(in->arg.emissive.color);
			out->arg.emissive.strength = value_deepcopy(in->arg.emissive.strength);
			break;
		case cr_bsdf_translucent:
			out->arg.translucent.color = color_deepcopy(in->arg.translucent.color);
			break;
		case cr_bsdf_background:
			out->arg.background.color = color_deepcopy(in->arg.background.color);
			out->arg.background.pose = vector_deepcopy(in->arg.background.pose);
			out->arg.background.strength = value_deepcopy(in->arg.background.strength);
		default:
			break;

	}

	return out;
}

// TODO: Remove once not needed anymore, and mark serialize_shader_node as static again
// #define NODE_DEBUG

#ifdef NODE_DEBUG
#include "../../common/vendored/cJSON.h"
cJSON *serialize_shader_node(const struct cr_shader_node *in);
#endif

static void debug_dump_node_tree(const struct cr_shader_node *desc) {
#ifdef NODE_DEBUG
	if (desc) {
		cJSON *debug = serialize_shader_node(desc);
		printf("%s\n", cJSON_Print(debug));
		cJSON_Delete(debug);
	} else {
		printf("NULL\n");
	}
#else
	(void)desc;
#endif
}

cr_material cr_material_set_add(struct cr_scene *s_ext, cr_material_set set, struct cr_shader_node *desc) {
	if (!s_ext) return -1;
	struct world *s = (struct world *)s_ext;
	if ((size_t)set > s->shader_buffers.count - 1) return -1;
	debug_dump_node_tree(desc);
	struct bsdf_buffer *buf = &s->shader_buffers.items[set];
	const struct bsdfNode *node = build_bsdf_node(s_ext, desc);
	cr_shader_node_ptr_arr_add(&buf->descriptions, shader_deepcopy(desc));
	return bsdf_node_ptr_arr_add(&buf->bsdfs, node);
}

void cr_material_update(struct cr_scene *s_ext, cr_material_set set, cr_material mat, struct cr_shader_node *desc) {
	if (!s_ext) return;
	struct world *s = (struct world *)s_ext;
	if ((size_t)set > s->shader_buffers.count - 1) return;
	struct bsdf_buffer *buf = &s->shader_buffers.items[set];
	if ((size_t)mat > buf->descriptions.count - 1) return;
	// struct bsdfNode *old_node = buf->bsdfs.items[mat];
	buf->bsdfs.items[mat] = build_bsdf_node(s_ext, desc);
	struct cr_shader_node *old_desc = buf->descriptions.items[mat];
	cr_shader_node_free(old_desc);
	buf->descriptions.items[mat] = shader_deepcopy(desc);
}

void cr_renderer_render(struct cr_renderer *ext) {
	if (!ext) return;
	struct renderer *r = (struct renderer *)ext;
	if (r->prefs.node_list) {
		// Wait for textures to finish decoding before syncing
		thread_pool_wait(r->scene->bg_worker);
		r->state.clients = clients_sync(r);
	}
	if (!r->state.clients.count && !r->prefs.threads) {
		return;
	}
	renderer_render(r);
}

void cr_renderer_start_interactive(struct cr_renderer *ext) {
	if (!ext) return;
	struct renderer *r = (struct renderer *)ext;
	r->prefs.iterative = true;
	if (!r->prefs.threads) {
		return;
	}
	renderer_start_interactive(r);
}

void cr_renderer_restart_interactive(struct cr_renderer *ext) {
	if (!ext) return;
	struct renderer *r = (struct renderer *)ext;
	if (!r->prefs.iterative) return;
	if (!r->state.workers.count) return;
	if (!r->state.result_buf) return;
	if (!r->state.current_set) return;
	struct camera *cam = &r->scene->cameras.items[r->prefs.selected_camera];
	if (r->state.result_buf->width != (size_t)cam->width || r->state.result_buf->height != (size_t)cam->height) {
		// Resize result buffer. First, pause render threads and wait for them to ack
		cam_recompute_optics(cam);
		logr(info, "Resizing result_buf (%zu,%zu) -> (%d,%d)\n", r->state.result_buf->width, r->state.result_buf->height, cam->width, cam->height);
		/*
			FIXME: Horrible, horrible hacks. Use proper pthreads primitives for this
			instead of hacky signal flags and busy loops.
		*/
		cr_renderer_toggle_pause((struct cr_renderer *)r);
		for (size_t i = 0; i < r->state.workers.count; ++i) {
			while (!r->state.workers.items[i].in_pause_loop) {
				timer_sleep_ms(1);
				if (r->state.s != r_rendering) {
					// Renderer stopped, bail out.
					cr_renderer_toggle_pause((struct cr_renderer *)r);
					return;
				}
			}
		}
		// Okay, threads are now paused, swap the buffer
		tex_destroy(r->state.result_buf);
		r->state.result_buf = tex_new(float_p, cam->width, cam->height, 4);

		// And patch in a new set of tiles.
		struct render_tile_arr new = tile_quantize(cam->width, cam->height, r->prefs.tileWidth, r->prefs.tileHeight, r->prefs.tileOrder);
		mutex_lock(r->state.current_set->tile_mutex);
		render_tile_arr_free(&r->state.current_set->tiles);
		r->state.current_set->tiles = new;
		r->state.current_set->finished = 0;
		mutex_release(r->state.current_set->tile_mutex);

		cr_renderer_toggle_pause((struct cr_renderer *)r);
	}
	// sus
	r->state.finishedPasses = 1;
	mutex_lock(r->state.current_set->tile_mutex);
	tex_clear(r->state.result_buf);
	r->state.current_set->finished = 0;
	for (size_t i = 0; i < r->prefs.threads; ++i) {
		// FIXME: Use array for workers
		// FIXME: What about network renderers?
		r->state.workers.items[i].totalSamples = 0;
	}
	update_toplevel_bvh(r->scene);
	// Why are we waiting for bg_worker? update_toplevel_bvh() is synchronous.
	thread_pool_wait(r->scene->bg_worker);
	mutex_release(r->state.current_set->tile_mutex);
}

struct cr_bitmap *cr_renderer_get_result(struct cr_renderer *ext) {
	if (!ext) return NULL;
	struct renderer *r = (struct renderer *)ext;
	return (struct cr_bitmap *)r->state.result_buf;
}

void cr_start_render_worker(int port, size_t thread_limit) {
	worker_start(port, thread_limit);
}

void cr_send_shutdown_to_workers(const char *node_list) {
	clients_shutdown(node_list);
}

bool cr_load_json(struct cr_renderer *r_ext, const char *file_path) {
	if (!r_ext || !file_path) return false;
	file_data input_bytes = file_load(file_path);
	if (!input_bytes.count) return false;
	char *asset_path = get_file_path(file_path);
	cr_renderer_set_str_pref(r_ext, cr_renderer_asset_path, asset_path);
	free(asset_path);
	cJSON *input = cJSON_ParseWithLength((const char *)input_bytes.items, input_bytes.count);
	if (parse_json(r_ext, input) < 0) {
		return false;
	}
	return true;
}

void cr_log_level_set(enum cr_log_level level) {
	log_level_set(level);
}

enum cr_log_level cr_log_level_get(void) {
	return log_level_get();
}

CR_EXPORT void cr_debug_dump_state(struct cr_renderer *r_ext) {
	if (!r_ext) return;
	struct renderer *r = (struct renderer *)r_ext;
	dump_renderer_state(r);
}
