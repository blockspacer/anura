#include <boost/array.hpp>
#include <boost/bind.hpp>
#include <boost/shared_ptr.hpp>
#include <boost/shared_array.hpp>
#include <boost/intrusive_ptr.hpp>
#include <boost/scoped_ptr.hpp>

#include <assert.h>

#include <algorithm>
#include <deque>
#include <numeric>
#include <map>
#include <vector>

#include "SDL.h"

#include "border_widget.hpp"
#include "button.hpp"
#include "camera.hpp"
#include "checkbox.hpp"
#include "color_picker.hpp"
#include "color_utils.hpp"
#include "dialog.hpp"
#include "filesystem.hpp"
#include "formatter.hpp"
#include "gles2.hpp"
#include "grid_widget.hpp"
#include "gui_section.hpp"
#include "isotile.hpp"
#include "json_parser.hpp"
#include "label.hpp"
#include "level_runner.hpp"
#include "module.hpp"
#include "preferences.hpp"
#include "unit_test.hpp"

#if defined(_MSC_VER)
#include <boost/math/special_functions/round.hpp>
#define bmround	boost::math::round
#else
#define bmround	round
#endif

#ifdef USE_GLES2

#define EXT_CALL(call) call
#define EXT_MACRO(macro) macro

namespace {
typedef boost::array<int, 3> VoxelPos;
struct Voxel {
	Voxel() : nlayer(-1) {}
	graphics::color color;
	int nlayer;
};

typedef std::map<VoxelPos, Voxel> VoxelMap;
typedef std::pair<VoxelPos, Voxel> VoxelPair;

variant write_voxel(const VoxelPair& p) {
	std::map<variant,variant> m;
	std::vector<variant> pos;
	for(int n = 0; n != 3; ++n) {
		pos.push_back(variant(p.first[n]));
	}
	m[variant("loc")] = variant(&pos);
	m[variant("color")] = p.second.color.write();
	return variant(&m);
}

VoxelPair read_voxel(const variant& v) {
	const std::vector<int>& pos = v["loc"].as_list_int();
	ASSERT_LOG(pos.size() == 3, "Bad location: " << v.write_json() << v.debug_location());

	VoxelPair result;

	std::copy(pos.begin(), pos.end(), &result.first[0]);
	result.second.color = graphics::color(v["color"]);
	return result;
}

struct Layer {
	std::string name;
	VoxelMap map;
};

struct LayerType {
	std::string name;
	std::map<std::string, Layer> variations;
	std::string last_edited_variation;
};

struct Model {
	std::vector<LayerType> layer_types;
};

LayerType read_layer_type(const variant& v) {
	LayerType result;
	result.last_edited_variation = v["last_edited_variation"].as_string_default();
	variant layers_node = v["variations"];
	if(layers_node.is_null()) {
		Layer default_layer;
		default_layer.name = "default";
		result.variations["default"] = default_layer;
		return result;
	}

	for(const std::pair<variant,variant>& p : layers_node.as_map()) {
		Layer layer;
		layer.name = p.first.as_string();
		variant layer_node = p.second;
		if(layer_node["voxels"].is_list()) {
			foreach(variant v, layer_node["voxels"].as_list()) {
				layer.map.insert(read_voxel(v));
			}
		}

		result.variations[layer.name] = layer;
	}

	return result;
}

Model read_model(const variant& v) {
	Model model;

	for(const std::pair<variant,variant>& p : v["layers"].as_map()) {
		LayerType layer_type = read_layer_type(p.second);
		layer_type.name = p.first.as_string();
		model.layer_types.push_back(layer_type);
	}

	return model;
}

variant write_model(const Model& model) {
	std::map<variant,variant> layers_node;
	for(const LayerType& layer_type : model.layer_types) {
		std::map<variant,variant> layer_type_node;
		layer_type_node[variant("name")] = variant(layer_type.name);
		layer_type_node[variant("last_edited_variation")] = variant(layer_type.last_edited_variation);

		std::map<variant,variant> variations_node;
		for(const std::pair<std::string, Layer>& p : layer_type.variations) {
			std::map<variant,variant> layer_node;
			layer_node[variant("name")] = variant(p.first);
			std::vector<variant> voxels;
			for(const VoxelPair& vp : p.second.map) {
				voxels.push_back(write_voxel(vp));
			}
			layer_node[variant("voxels")] = variant(&voxels);
			variations_node[variant(p.first)] = variant(&layer_node);
		}

		layer_type_node[variant("variations")] = variant(&variations_node);
		layers_node[variant(layer_type.name)] = variant(&layer_type_node);
	}

	std::map<variant,variant> result_node;
	result_node[variant("layers")] = variant(&layers_node);
	return variant(&result_node);
}

struct Command {
	Command(std::function<void()> redo_fn, std::function<void()> undo_fn)
	  : redo(redo_fn), undo(undo_fn)
	{}
	std::function<void()> redo, undo;
};

const char* ToolIcons[] = {
	"editor_pencil",
	"editor_add_object",
	"editor_eyedropper",
	NULL
};

enum VOXEL_TOOL {
	TOOL_PENCIL,
	TOOL_PENCIL_ABOVE,
	TOOL_PICKER,
	NUM_VOXEL_TOOLS,
};

class iso_renderer;

class voxel_editor : public gui::dialog
{
public:
	voxel_editor(const rect& r, const std::string& fname);
	~voxel_editor();
	void init();

	const VoxelMap& voxels() const { return voxels_; }

	void set_voxel(const VoxelPos& pos, const Voxel& voxel);
	void delete_voxel(const VoxelPos& pos);
	bool set_cursor(const VoxelPos& pos);

	const VoxelPos* get_cursor() const { return cursor_.get(); }

	VoxelPos get_selected_voxel(const VoxelPos& pos, int facing, bool reverse);

	graphics::color current_color() const { return color_picker_->get_selected_color(); }
	gui::color_picker& get_color_picker() { return *color_picker_; }
	Layer& layer() { assert(current_layer_ >= 0 && current_layer_ < layers_.size()); return layers_[current_layer_]; }

	int nhighlight_layer() const { return highlight_layer_; }

	VOXEL_TOOL tool() const {
		const bool ctrl = (SDL_GetModState()&KMOD_CTRL) != 0;
		const bool shift = (SDL_GetModState()&KMOD_SHIFT) != 0;
		if(shift && tool_ == TOOL_PENCIL) {
			return TOOL_PENCIL_ABOVE;
		} else if(ctrl && (tool_ == TOOL_PENCIL || tool_ == TOOL_PENCIL_ABOVE)) {
			return TOOL_PICKER;
		}

		return tool_;
	}

	void execute_command(std::function<void()> redo, std::function<void()> undo);
	void execute_command(const Command& cmd);
private:
	bool handle_event(const SDL_Event& event, bool claimed);

	void on_color_changed(const graphics::color& color);

	void select_tool(VOXEL_TOOL tool);

	void set_symmetric(bool value);

	void mouseover_layer(int nlayer);
	void select_layer(int nlayer, gui::grid* layer_grid);

	void on_save();
	void undo();
	void redo();

	void handle_process();

	const Layer& layer() const { return layers_[current_layer_]; }

	void build_voxels();

	rect area_;

	int current_layer_, highlight_layer_;
	std::vector<Layer> layers_;
	Model model_;
	VoxelMap voxels_;

	boost::scoped_ptr<VoxelPos> cursor_;

	gui::label_ptr pos_label_;

	std::string fname_;

	boost::intrusive_ptr<iso_renderer> iso_renderer_;
	boost::intrusive_ptr<gui::color_picker> color_picker_;

	std::vector<Command> undo_, redo_;

	VOXEL_TOOL tool_;

	std::vector<gui::border_widget*> tool_borders_;

	bool symmetric_;
};

voxel_editor* g_voxel_editor;

voxel_editor& get_editor() {
	assert(g_voxel_editor);
	return *g_voxel_editor;
}

using namespace gui;

class iso_renderer : public gui::widget
{
public:
	explicit iso_renderer(const rect& area);
	~iso_renderer();
	void handle_draw() const;

	const camera_callable& camera() const { return *camera_; }
private:
	void init();
	void handle_process();
	bool handle_event(const SDL_Event& event, bool claimed);

	glm::ivec3 position_to_cube(int xp, int yp, glm::ivec3* facing);

	void render_fbo();
	boost::intrusive_ptr<camera_callable> camera_;
	GLfloat camera_hangle_, camera_vangle_, camera_distance_;

	void calculate_camera();

	boost::shared_array<GLuint> fbo_texture_ids_;
	glm::mat4 fbo_proj_;
	boost::shared_ptr<GLuint> framebuffer_id_;
	boost::shared_ptr<GLuint> depth_id_;

	boost::array<GLfloat, 3> vector_;

	size_t tex_width_;
	size_t tex_height_;
	GLint video_framebuffer_id_;

	bool focused_;

	iso_renderer();
	iso_renderer(const iso_renderer&);
};

iso_renderer* g_iso_renderer;
iso_renderer& get_iso_renderer() {
	assert(g_iso_renderer);
	return *g_iso_renderer;
}

iso_renderer::iso_renderer(const rect& area)
  : camera_(new camera_callable),
    camera_hangle_(0.12), camera_vangle_(1.25), camera_distance_(20.0),
	tex_width_(0), tex_height_(0),
	focused_(false)
{
	camera_->set_clip_planes(0.1f, 200.0f);
	g_iso_renderer = this;
	set_loc(area.x(), area.y());
	set_dim(area.w(), area.h());
	vector_[0] = 1.0;
	vector_[1] = 1.0;
	vector_[2] = 1.0;

	calculate_camera();

	init();
}

iso_renderer::~iso_renderer()
{
	if(g_iso_renderer == this) {
		g_iso_renderer = NULL;
	}
}

void iso_renderer::calculate_camera()
{
	const GLfloat hdist = sin(camera_vangle_)*camera_distance_;
	const GLfloat ydist = cos(camera_vangle_)*camera_distance_;

	const GLfloat xdist = sin(camera_hangle_)*hdist;
	const GLfloat zdist = cos(camera_hangle_)*hdist;

	std::cerr << "LOOK AT: " << xdist << ", " << ydist << ", " << zdist << "\n";

	camera_->look_at(glm::vec3(xdist, ydist, zdist), glm::vec3(0,0,0), glm::vec3(0.0, 1.0, 0.0));
}

void iso_renderer::handle_draw() const
{
	gles2::manager gles2_manager(gles2::shader_program::get_global("texture2d"));

	GLint cur_id = graphics::texture::get_current_texture();
	glBindTexture(GL_TEXTURE_2D, fbo_texture_ids_[0]);

	const int w_odd = width() % 2;
	const int h_odd = height() % 2;
	const int w = width() / 2;
	const int h = height() / 2;

	glm::mat4 mvp = fbo_proj_ * glm::translate(glm::mat4(1.0f), glm::vec3(x()+w, y()+h, 0.0f));
	glUniformMatrix4fv(gles2::active_shader()->shader()->mvp_matrix_uniform(), 1, GL_FALSE, glm::value_ptr(mvp));

	GLfloat varray[] = {
		-w, -h,
		-w, h+h_odd,
		w+w_odd, -h,
		w+w_odd, h+h_odd
	};
	const GLfloat tcarray[] = {
		0.0f, GLfloat(height())/tex_height_,
		0.0f, 0.0f,
		GLfloat(width())/tex_width_, GLfloat(height())/tex_height_,
		GLfloat(width())/tex_width_, 0.0f,
	};
	gles2::active_shader()->shader()->vertex_array(2, GL_FLOAT, 0, 0, varray);
	gles2::active_shader()->shader()->texture_array(2, GL_FLOAT, 0, 0, tcarray);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

	glBindTexture(GL_TEXTURE_2D, cur_id);
}

void iso_renderer::handle_process()
{
	int num_keys = 0;
	const Uint8* keystate = SDL_GetKeyboardState(&num_keys);
	if(SDL_SCANCODE_Z < num_keys && keystate[SDL_SCANCODE_Z]) {
		camera_distance_ -= 0.2;
		if(camera_distance_ < 5.0) {
			camera_distance_ = 5.0;
		}

		calculate_camera();
	}

	if(SDL_SCANCODE_X < num_keys && keystate[SDL_SCANCODE_X]) {
		camera_distance_ += 0.2;
		if(camera_distance_ > 100.0) {
			camera_distance_ = 100.0;
		}

		calculate_camera();
	}

	render_fbo();
}

glm::ivec3 iso_renderer::position_to_cube(int xp, int yp, glm::ivec3* facing)
{
	// Before calling screen_to_world we need to bind the fbo
	EXT_CALL(glBindFramebuffer)(EXT_MACRO(GL_FRAMEBUFFER), *framebuffer_id_);
	glm::vec3 world_coords = graphics::screen_to_world(camera_, xp, yp, width(), height());
	EXT_CALL(glBindFramebuffer)(EXT_MACRO(GL_FRAMEBUFFER), video_framebuffer_id_);
	glm::ivec3 voxel_coord = glm::ivec3(
		abs(world_coords[0]-bmround(world_coords[0])) < 0.05f ? int(bmround(world_coords[0])) : int(world_coords[0]),
		abs(world_coords[1]-bmround(world_coords[1])) < 0.05f ? int(bmround(world_coords[1])) : int(world_coords[1]),
		abs(world_coords[2]-bmround(world_coords[2])) < 0.05f ? int(bmround(world_coords[2])) : int(world_coords[2]));
	*facing = isometric::get_facing(camera_, world_coords);
	if(facing->x > 0) {
		--voxel_coord.x; 
	}
	if(facing->y > 0) {
		--voxel_coord.y; 
	}
	if(facing->z > 0) {
		--voxel_coord.z; 
	}
	return voxel_coord;
}

bool iso_renderer::handle_event(const SDL_Event& event, bool claimed)
{
	switch(event.type) {
	case SDL_MOUSEWHEEL: {
		if(!focused_) {
			break;
		}

		if(event.wheel.y > 0) {
			camera_distance_ -= 5.0;
			if(camera_distance_ < 5.0) {
				camera_distance_ = 5.0;
			}

			calculate_camera();
		} else {
			camera_distance_ += 5.0;
			if(camera_distance_ > 100.0) {
				camera_distance_ = 100.0;
			}

			calculate_camera();
		}
		
		break;
	}
	case SDL_MOUSEMOTION: {
		const SDL_MouseMotionEvent& motion = event.motion;
		if(motion.x >= x() && motion.y >= y() &&
		   motion.x <= x() + width() && motion.y <= y() + height()) {
			focused_ = true;
						
			glm::ivec3 facing;
			glm::ivec3 voxel_coord = position_to_cube(motion.x-x(), motion.y-y(), &facing);

			VoxelPos pos = {voxel_coord.x, voxel_coord.y, voxel_coord.z};
			auto it = get_editor().voxels().find(pos);
			if(it != get_editor().voxels().end()) {
				if(SDL_GetModState()&KMOD_CTRL) {
					glm::ivec3 new_coord = voxel_coord + facing;
					pos[0] = new_coord.x;
					pos[1] = new_coord.y;
					pos[2] = new_coord.z;
				}
				get_editor().set_cursor(pos);
			} else {
				Uint8 button_state = SDL_GetMouseState(NULL, NULL);
				if(button_state & SDL_BUTTON(SDL_BUTTON_LEFT)) {
					if(motion.xrel) {
						camera_hangle_ += motion.xrel*0.02;
					}

					if(motion.yrel) {
						camera_vangle_ += motion.yrel*0.02;
					}

					std::cerr << "ANGLE: " << camera_hangle_ << ", " << camera_vangle_ << "\n";

					calculate_camera();
				}
			}
		} else {
			focused_ = false;
		}
		break;
	}
	}

	return widget::handle_event(event, claimed);
}

void iso_renderer::init()
{
	fbo_proj_ = glm::ortho(0.0f, float(preferences::actual_screen_width()), float(preferences::actual_screen_height()), 0.0f);

	tex_width_ = graphics::texture::allows_npot() ? width() : graphics::texture::next_power_of_2(width());
	tex_height_ = graphics::texture::allows_npot() ? height() : graphics::texture::next_power_of_2(height());

	glGetIntegerv(EXT_MACRO(GL_FRAMEBUFFER_BINDING), &video_framebuffer_id_);

	glDepthFunc(GL_LEQUAL);
	glDepthMask(GL_TRUE);

	fbo_texture_ids_ = boost::shared_array<GLuint>(new GLuint[1], [](GLuint* id){glDeleteTextures(1,id); delete id;});
	glGenTextures(1, &fbo_texture_ids_[0]);
	glBindTexture(GL_TEXTURE_2D, fbo_texture_ids_[0]);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, tex_width_, tex_height_, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glBindTexture(GL_TEXTURE_2D, 0);

	framebuffer_id_ = boost::shared_ptr<GLuint>(new GLuint, [](GLuint* id){glDeleteFramebuffers(1, id); delete id;});
	EXT_CALL(glGenFramebuffers)(1, framebuffer_id_.get());
	EXT_CALL(glBindFramebuffer)(EXT_MACRO(GL_FRAMEBUFFER), *framebuffer_id_);

	// attach the texture to FBO color attachment point
	EXT_CALL(glFramebufferTexture2D)(EXT_MACRO(GL_FRAMEBUFFER), EXT_MACRO(GL_COLOR_ATTACHMENT0),
                          GL_TEXTURE_2D, fbo_texture_ids_[0], 0);
	depth_id_ = boost::shared_ptr<GLuint>(new GLuint, [](GLuint* id){glBindRenderbuffer(GL_RENDERBUFFER, 0); glDeleteRenderbuffers(1, id); delete id;});
	glGenRenderbuffers(1, depth_id_.get());
	glBindRenderbuffer(GL_RENDERBUFFER, *depth_id_);
	glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT, tex_width_, tex_height_);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, *depth_id_);

	// check FBO status
	GLenum status = EXT_CALL(glCheckFramebufferStatus)(EXT_MACRO(GL_FRAMEBUFFER));
	ASSERT_NE(status, EXT_MACRO(GL_FRAMEBUFFER_UNSUPPORTED));
	ASSERT_EQ(status, EXT_MACRO(GL_FRAMEBUFFER_COMPLETE));
}

void iso_renderer::render_fbo()
{
	EXT_CALL(glBindFramebuffer)(EXT_MACRO(GL_FRAMEBUFFER), *framebuffer_id_);

	//set up the raster projection.
	glViewport(0, 0, width(), height());

	glClearColor(0.0, 0.0, 0.0, 0.0);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

	glColor4f(1.0, 1.0, 1.0, 1.0);

	glEnable(GL_DEPTH_TEST);

	//start drawing here.
	gles2::shader_program_ptr shader_program(gles2::shader_program::get_global("iso_color_line"));
	gles2::program_ptr shader = shader_program->shader();
	gles2::actives_map_iterator mvp_uniform_itor = shader->get_uniform_reference("mvp_matrix");

	gles2::manager gles2_manager(shader_program);

	glm::mat4 model_matrix(1.0f);

	glm::mat4 mvp = camera_->projection_mat() * camera_->view_mat() * model_matrix;

	shader->set_uniform(mvp_uniform_itor, 1, glm::value_ptr(mvp));

	////////////////////////////////////////////////////////////////////////////
	// Lighting stuff.
	static GLuint LightPosition_worldspace = -1;
	if(LightPosition_worldspace == -1) {
		LightPosition_worldspace = shader->get_uniform("LightPosition_worldspace");
	}
	glUniform3f(LightPosition_worldspace, 0.0f, 20.0f, 100.0f);
	static GLuint LightPower = -1;
	if(LightPower == -1) {
		LightPower = shader->get_uniform("LightPower");
	}
	glUniform1f(LightPower, 7000.0f);
	static GLuint m_matrix = -1;
	if(m_matrix == -1) {
		m_matrix = shader->get_uniform("m_matrix");
	}
	glUniformMatrix4fv(m_matrix, 1, GL_FALSE, glm::value_ptr(model_matrix));
	static GLuint v_matrix = -1;
	if(v_matrix == -1) {
		v_matrix = shader->get_uniform("v_matrix");
	}
	glUniformMatrix4fv(v_matrix, 1, GL_FALSE, camera_->view());
	static GLuint a_normal = -1;
	if(a_normal == -1) {
		a_normal = shader->get_attribute("a_normal");
	}
	////////////////////////////////////////////////////////////////////////////

	std::vector<GLfloat> varray, carray, narray;

	const GLfloat axes_vertex[] = {
		0.0, 0.0, 0.0,
		0.0, 0.0, 10.0,
		0.0, 0.0, 0.0,
		0.0, 10.0, 0.0,
		0.0, 0.0, 0.0,
		10.0, 0.0, 0.0,
	};

	for(int n = 0; n != sizeof(axes_vertex)/sizeof(*axes_vertex); ++n) {
		varray.push_back(axes_vertex[n]);
		if(n%3 == 0) {
			carray.push_back(1.0);
			carray.push_back(1.0);
			carray.push_back(1.0);
			carray.push_back(1.0);
		}
	}

	if(get_editor().get_cursor()) {
		const VoxelPos& cursor = *get_editor().get_cursor();
		const GLfloat cursor_vertex[] = {
			cursor[0], cursor[1], cursor[2],
			cursor[0]+1.0, cursor[1], cursor[2],
			cursor[0]+1.0, cursor[1], cursor[2],
			cursor[0]+1.0, cursor[1]+1.0, cursor[2],
			cursor[0]+1.0, cursor[1]+1.0, cursor[2],
			cursor[0], cursor[1]+1.0, cursor[2],
			cursor[0], cursor[1]+1.0, cursor[2],
			cursor[0], cursor[1], cursor[2],

			cursor[0], cursor[1], cursor[2]+1.0,
			cursor[0]+1.0, cursor[1], cursor[2]+1.0,
			cursor[0]+1.0, cursor[1], cursor[2]+1.0,
			cursor[0]+1.0, cursor[1]+1.0, cursor[2]+1.0,
			cursor[0]+1.0, cursor[1]+1.0, cursor[2]+1.0,
			cursor[0], cursor[1]+1.0, cursor[2]+1.0,
			cursor[0], cursor[1]+1.0, cursor[2]+1.0,
			cursor[0], cursor[1], cursor[2]+1.0,

			cursor[0], cursor[1], cursor[2],
			cursor[0], cursor[1], cursor[2]+1.0,
			cursor[0]+1.0, cursor[1], cursor[2],
			cursor[0]+1.0, cursor[1], cursor[2]+1.0,
			cursor[0]+1.0, cursor[1]+1.0, cursor[2],
			cursor[0]+1.0, cursor[1]+1.0, cursor[2]+1.0,
			cursor[0], cursor[1]+1.0, cursor[2],
			cursor[0], cursor[1]+1.0, cursor[2]+1.0,
		};

		for(int n = 0; n != sizeof(cursor_vertex)/sizeof(*cursor_vertex); ++n) {
			varray.push_back(cursor_vertex[n]);
			if(n%3 == 0) {
				carray.push_back(1.0);
				carray.push_back(1.0);
				carray.push_back(0.0);
				carray.push_back(1.0);
			}
		}
	}

	gles2::active_shader()->shader()->vertex_array(3, GL_FLOAT, 0, 0, &varray[0]);
	gles2::active_shader()->shader()->color_array(4, GL_FLOAT, 0, 0, &carray[0]);
	glDrawArrays(GL_LINES, 0, varray.size()/3);

	varray.clear();
	carray.clear();
	narray.clear();

	for(const VoxelPair& p : get_editor().voxels()) {
		const VoxelPos& pos = p.first;

		const GLfloat vertex[] = {
			0, 0, 0, // back face lower
			1, 0, 0,
			1, 1, 0,

			0, 0, 0, // back face upper
			0, 1, 0,
			1, 1, 0,

			0, 0, 1, // front face lower
			1, 0, 1,
			1, 1, 1,

			0, 0, 1, // front face upper
			0, 1, 1,
			1, 1, 1,

			0, 0, 0, // left face upper
			0, 1, 0,
			0, 1, 1,

			0, 0, 0, // left face lower
			0, 0, 1,
			0, 1, 1,

			1, 0, 0, // right face upper
			1, 1, 0,
			1, 1, 1,

			1, 0, 0, // right face lower
			1, 0, 1,
			1, 1, 1,

			0, 0, 0, // bottom face right
			1, 0, 0,
			1, 0, 1,

			0, 0, 0, // bottom face left
			0, 0, 1,
			1, 0, 1,

			0, 1, 0, // top face right
			1, 1, 0,
			1, 1, 1,

			0, 1, 0, // top face left
			0, 1, 1,
			1, 1, 1,
		};

		const GLfloat normal [] =
		{
			0, 0, -1,
			0, 0, -1,
			0, 0, -1,

			0, 0, -1,
			0, 0, -1,
			0, 0, -1,

			0, 0, 1,
			0, 0, 1,
			0, 0, 1,

			0, 0, 1,
			0, 0, 1,
			0, 0, 1,

			-1, 0, 0,
			-1, 0, 0,
			-1, 0, 0,

			-1, 0, 0,
			-1, 0, 0,
			-1, 0, 0,

			1, 0, 0,
			1, 0, 0,
			1, 0, 0,

			1, 0, 0,
			1, 0, 0,
			1, 0, 0,

			0, -1, 0,
			0, -1, 0,
			0, -1, 0,

			0, -1, 0,
			0, -1, 0,
			0, -1, 0,

			0, 1, 0,
			0, 1, 0,
			0, 1, 0,

			0, 1, 0,
			0, 1, 0,
			0, 1, 0,
		};

		graphics::color color = p.second.color;
		const bool is_selected = get_editor().get_cursor() && *get_editor().get_cursor() == pos || get_editor().nhighlight_layer() >= 0 && p.second.nlayer == get_editor().nhighlight_layer();
		if(is_selected) {
			const int delta = sin(SDL_GetTicks()*0.01)*64;
			graphics::color_transform transform(delta, delta, delta, 0);
			graphics::color_transform new_color = graphics::color_transform(color) + transform;
			color = new_color.to_color();
		}

		int face = 0;

		for(int n = 0; n != sizeof(vertex)/sizeof(*vertex); ++n) {
			varray.push_back(pos[n%3]+vertex[n]);
			narray.push_back(normal[n]);
			if(n%3 == 0) {
				carray.push_back(color.r()/255.0f); 
				carray.push_back(color.g()/255.0f); 
				carray.push_back(color.b()/255.0f); 
				carray.push_back(color.a()/255.0f);
			}
		}
	}

	if(!varray.empty()) {
		shader->vertex_array(3, GL_FLOAT, GL_FALSE, 0, &varray[0]);
		shader->color_array(4, GL_FLOAT, GL_FALSE, 0, &carray[0]);
		shader->vertex_attrib_array(a_normal, 3, GL_FLOAT, GL_FALSE, 0, &narray[0]);
		glDrawArrays(GL_TRIANGLES, 0, varray.size()/3);
	}

	EXT_CALL(glBindFramebuffer)(EXT_MACRO(GL_FRAMEBUFFER), video_framebuffer_id_);

	glViewport(0, 0, preferences::actual_screen_width(), preferences::actual_screen_height());

	glDisable(GL_DEPTH_TEST);
}

class perspective_renderer : public gui::widget
{
public:
	perspective_renderer(int xdir, int ydir, int zdir);
	void handle_draw() const;

	void zoom_in();
	void zoom_out();

	//converts given pos to [x,y,0]
	VoxelPos normalize_pos(const VoxelPos& pos) const;

	VoxelPos denormalize_pos(const VoxelPos& pos) const;
private:
	VoxelPos get_mouse_pos(int mousex, int mousey) const;
	bool handle_event(const SDL_Event& event, bool claimed);
	bool calculate_cursor(int mousex, int mousey);
	void pencil_voxel();
	void delete_voxel();

	bool is_flipped() const { return vector_[0] + vector_[1] + vector_[2] < 0; }
	int vector_[3];
	int facing_; //0=x, 1=y, 2=z
	int voxel_width_;

	int last_select_x_, last_select_y_;

	int invert_y_;

	bool drawing_on_;
	std::set<VoxelPos> voxels_drawn_on_this_drag_;

	bool focus_;
};

perspective_renderer::perspective_renderer(int xdir, int ydir, int zdir)
  : voxel_width_(20), last_select_x_(INT_MIN), last_select_y_(INT_MIN),
    invert_y_(1), drawing_on_(false), focus_(false)
{
	vector_[0] = xdir;
	vector_[1] = ydir;
	vector_[2] = zdir;

	for(int n = 0; n != 3; ++n) {
		if(vector_[n]) {
			facing_ = n;
			break;
		}
	}

	if(facing_ != 1) {
		invert_y_ *= -1;
	}
};

void perspective_renderer::zoom_in()
{
	if(voxel_width_ < 80) {
		voxel_width_ *= 2;
	}
}

void perspective_renderer::zoom_out()
{
	if(voxel_width_ > 5) {
		voxel_width_ /= 2;
	}
}

VoxelPos perspective_renderer::normalize_pos(const VoxelPos& pos) const
{
	VoxelPos result;
	result[2] = 0;
	int* out = &result[0];

	int dimensions[3] = {0, 2, 1};
	for(int n = 0; n != 3; ++n) {
		if(dimensions[n] != facing_) {
			*out++ = pos[dimensions[n]];
		}
	}

	return result;
}

VoxelPos perspective_renderer::denormalize_pos(const VoxelPos& pos2d) const
{
	const int* p = &pos2d[0];

	VoxelPos pos;
	int dimensions[3] = {0,2,1};
	for(int n = 0; n != 3; ++n) {
		if(dimensions[n] != facing_) {
			pos[dimensions[n]] = *p++;
		} else {
			pos[dimensions[n]] = 0;
		}
	}

	return pos;
}

VoxelPos perspective_renderer::get_mouse_pos(int mousex, int mousey) const
{
	int xpos = mousex - (x() + width()/2);
	int ypos = mousey - (y() + height()/2);

	if(xpos < 0) {
		xpos -= voxel_width_;
	}

	if(ypos > 0) {
		ypos += voxel_width_;
	}

	const int xselect = xpos/voxel_width_;
	const int yselect = ypos/voxel_width_;
	VoxelPos result;
	result[0] = xselect;
	result[1] = yselect*invert_y_;
	result[2] = 0;
	return result;
}

void perspective_renderer::pencil_voxel()
{
	if(get_editor().get_cursor()) {
		VoxelPos cursor = *get_editor().get_cursor();
		Voxel voxel;
		voxel.color = get_editor().current_color();

		Voxel old_voxel;
		bool currently_has_voxel = false;

		auto current_itor = get_editor().layer().map.find(cursor);
		if(current_itor != get_editor().layer().map.end()) {
			old_voxel = current_itor->second;
			currently_has_voxel = true;
		}

		get_editor().execute_command(
		  [cursor, voxel]() { get_editor().set_voxel(cursor, voxel); },
		  [cursor, old_voxel, currently_has_voxel]() {
			if(currently_has_voxel) {
				get_editor().set_voxel(cursor, old_voxel);
			} else {
				get_editor().delete_voxel(cursor);
			}
		});

		get_editor().set_voxel(cursor, voxel);
	}
}

void perspective_renderer::delete_voxel()
{
	if(get_editor().get_cursor()) {
		VoxelPos cursor = *get_editor().get_cursor();
		auto current_itor = get_editor().layer().map.find(cursor);
		if(current_itor == get_editor().layer().map.end()) {
			return;
		}

		Voxel old_voxel = current_itor->second;

		get_editor().execute_command(
			[cursor]() { get_editor().delete_voxel(cursor); },
			[cursor, old_voxel]() { get_editor().set_voxel(cursor, old_voxel); }
		);
	}
}

bool perspective_renderer::calculate_cursor(int mousex, int mousey)
{
	if(mousex == INT_MIN) {
		return false;
	}

	const VoxelPos pos2d = get_mouse_pos(mousex, mousey);
	const VoxelPos pos = denormalize_pos(pos2d);

	VoxelPos cursor = get_editor().get_selected_voxel(pos, facing_, vector_[facing_] < 0);
	if(get_editor().tool() == TOOL_PENCIL_ABOVE && get_editor().voxels().count(cursor)) {
		for(int n = 0; n != 3; ++n) {
			cursor[n] += vector_[n];
		}
	}

	return get_editor().set_cursor(cursor);

}

bool perspective_renderer::handle_event(const SDL_Event& event, bool claimed)
{
	switch(event.type) {
	case SDL_KEYUP:
	case SDL_KEYDOWN: {
		calculate_cursor(last_select_x_, last_select_y_);
		break;
	}

	case SDL_MOUSEWHEEL: {
		int mx, my;
		SDL_GetMouseState(&mx, &my);
		if(!focus_ || get_editor().get_cursor() == NULL) {
			break;
		}

		VoxelPos cursor = *get_editor().get_cursor();

		if(event.wheel.y > 0) {
			cursor[facing_] -= vector_[facing_];
		} else {
			cursor[facing_] += vector_[facing_];
		}
		get_editor().set_cursor(cursor);

		break;
	}

	case SDL_MOUSEBUTTONUP: {
		drawing_on_ = false;
		voxels_drawn_on_this_drag_.clear();
		break;
	}

	case SDL_MOUSEBUTTONDOWN: {
		const SDL_MouseButtonEvent& e = event.button;
		if(e.x >= x() && e.y >= y() &&
		   e.x <= x() + width() && e.y <= y() + height()) {
			switch(get_editor().tool()) {
			case TOOL_PENCIL:
			case TOOL_PENCIL_ABOVE: {
				if(e.button == SDL_BUTTON_LEFT) {
					pencil_voxel();
				} else if(e.button == SDL_BUTTON_RIGHT) {
					delete_voxel();
				}

				calculate_cursor(last_select_x_, last_select_y_);

				drawing_on_ = true;
				voxels_drawn_on_this_drag_.clear();

				if(get_editor().get_cursor()) {
					voxels_drawn_on_this_drag_.insert(normalize_pos(*get_editor().get_cursor()));
				}
				break;
			}

			case TOOL_PICKER: {
				if(get_editor().get_cursor()) {
					auto voxel_itor = get_editor().voxels().find(*get_editor().get_cursor());
					if(voxel_itor != get_editor().voxels().end()) {
						const graphics::color color = voxel_itor->second.color;
						if(e.button == SDL_BUTTON_LEFT) {
							get_editor().get_color_picker().set_primary_color(color);
						} else if(e.button == SDL_BUTTON_RIGHT) {
							get_editor().get_color_picker().set_secondary_color(color);
						}
					}
				}

				break;
			}
			default:
				break;
			}
		} else {
			drawing_on_ = false;
			voxels_drawn_on_this_drag_.clear();
		}
		break;
	}

	case SDL_MOUSEMOTION: {
		const SDL_MouseMotionEvent& motion = event.motion;
		if(motion.x >= x() && motion.y >= y() &&
		   motion.x <= x() + width() && motion.y <= y() + height()) {
			focus_ = true;

			const bool is_cursor_set = calculate_cursor(motion.x, motion.y);
			last_select_x_ = motion.x;
			last_select_y_ = motion.y;

			if(is_cursor_set) {
				Uint8 button_state = SDL_GetMouseState(NULL, NULL);
				switch(get_editor().tool()) {
				case TOOL_PENCIL: {
					if(button_state & SDL_BUTTON(SDL_BUTTON_LEFT) && drawing_on_) {
						if(voxels_drawn_on_this_drag_.count(normalize_pos(*get_editor().get_cursor())) == 0) {
							pencil_voxel();
							calculate_cursor(motion.x, motion.y);
							voxels_drawn_on_this_drag_.insert(normalize_pos(*get_editor().get_cursor()));
						}
					} else if(button_state & SDL_BUTTON(SDL_BUTTON_RIGHT) && drawing_on_) {
						if(voxels_drawn_on_this_drag_.count(normalize_pos(*get_editor().get_cursor())) == 0) {
							delete_voxel();
							calculate_cursor(motion.x, motion.y);
							voxels_drawn_on_this_drag_.insert(normalize_pos(*get_editor().get_cursor()));
						}
					}
					break;
				}

				default:
					break;
				}
			}

			break;
		} else {
			last_select_x_ = last_select_y_ = INT_MIN;
			focus_ = false;
		}
	}
	}
	return widget::handle_event(event, claimed);
}

void perspective_renderer::handle_draw() const
{
	const SDL_Rect clip_area = { x(), y(), width(), height() };
	const graphics::clip_scope clipping_scope(clip_area);

	gles2::manager gles2_manager(gles2::get_simple_col_shader());

	std::vector<GLfloat> varray, carray;

	const int cells_h = width()/voxel_width_ + 1;
	const int cells_v = height()/voxel_width_ + 1;
	for(int xpos = -cells_h/2; xpos <= cells_h/2; ++xpos) {
		const int left_side = x() + width()/2 + xpos*voxel_width_;
		if(left_side < x() || left_side + voxel_width_ > x() + width()) {
			continue;
		}

		varray.push_back(left_side);
		varray.push_back(y());
		varray.push_back(left_side);
		varray.push_back(y() + height());

		carray.push_back(1.0);
		carray.push_back(1.0);
		carray.push_back(1.0);
		carray.push_back(xpos == 0 ? 1.0 : 0.3);

		carray.push_back(1.0);
		carray.push_back(1.0);
		carray.push_back(1.0);
		carray.push_back(xpos == 0 ? 1.0 : 0.3);
	}

	for(int ypos = -cells_v/2; ypos <= cells_v/2; ++ypos) {
		const int top_side = y() + height()/2 + ypos*voxel_width_;
		if(top_side < y() || top_side + voxel_width_ > y() + height()) {
			continue;
		}

		varray.push_back(x());
		varray.push_back(top_side);
		varray.push_back(x() + width());
		varray.push_back(top_side);

		carray.push_back(1.0);
		carray.push_back(1.0);
		carray.push_back(1.0);
		carray.push_back(ypos == 0 ? 1.0 : 0.3);

		carray.push_back(1.0);
		carray.push_back(1.0);
		carray.push_back(1.0);
		carray.push_back(ypos == 0 ? 1.0 : 0.3);
	}

	if(get_editor().get_cursor()) {
		const VoxelPos cursor = normalize_pos(*get_editor().get_cursor());

		const int x1 = x() + width()/2 + cursor[0]*voxel_width_;
		const int y1 = y() + height()/2 + cursor[1]*voxel_width_*invert_y_;

		const int x2 = x1 + voxel_width_;
		const int y2 = y1 - voxel_width_;

		int vertexes[] = { x1, y1, x1, y2,
		                   x2, y1, x2, y2,
						   x1, y1, x2, y1,
						   x1, y2, x2, y2, };
		for(int n = 0; n != sizeof(vertexes)/sizeof(*vertexes); ++n) {
			varray.push_back(vertexes[n]);
			if(n%2 == 0) {
				carray.push_back(1.0);
				carray.push_back(0.0);
				carray.push_back(0.0);
				carray.push_back(1.0);
			}
		}
	}

	gles2::active_shader()->shader()->vertex_array(2, GL_FLOAT, 0, 0, &varray[0]);
	gles2::active_shader()->shader()->color_array(4, GL_FLOAT, 0, 0, &carray[0]);
	glDrawArrays(GL_LINES, 0, varray.size()/2);

	varray.clear();
	carray.clear();

	std::vector<VoxelPair> voxels(get_editor().voxels().begin(), get_editor().voxels().end());
	if(is_flipped()) {
		std::reverse(voxels.begin(), voxels.end());
	}

	for(const VoxelPair& p : voxels) {
		const VoxelPos pos = normalize_pos(p.first);

		const int x1 = x() + width()/2 + pos[0]*voxel_width_;
		const int y1 = y() + height()/2 + pos[1]*voxel_width_*invert_y_;

		const int x2 = x1 + voxel_width_;
		const int y2 = y1 - voxel_width_;

		bool is_selected = get_editor().get_cursor() && normalize_pos(*get_editor().get_cursor()) == pos || get_editor().nhighlight_layer() >= 0 && get_editor().nhighlight_layer() == p.second.nlayer;

		graphics::color color = p.second.color;
		if(is_selected) {
			const int delta = sin(SDL_GetTicks()*0.01)*64;
			graphics::color_transform transform(delta, delta, delta, 0);
			graphics::color_transform new_color = graphics::color_transform(color) + transform;
			color = new_color.to_color();
		}

		int vertexes[] = { x1, y1,
		                   x1, y1, x1, y2,
		                   x2, y1, x2, y2,
						   x1, y1, x2, y1,
						   x1, y2, x2, y2,
						   x2, y2, };
		for(int n = 0; n != sizeof(vertexes)/sizeof(*vertexes); ++n) {
			varray.push_back(vertexes[n]);
			if(n%2 == 0) {
				carray.push_back(color.r()/255.0);
				carray.push_back(color.g()/255.0);
				carray.push_back(color.b()/255.0);
				carray.push_back(color.a()/255.0);
			}
		}
	}

	gles2::active_shader()->shader()->vertex_array(2, GL_FLOAT, 0, 0, &varray[0]);
	gles2::active_shader()->shader()->color_array(4, GL_FLOAT, 0, 0, &carray[0]);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, varray.size()/2);

	varray.clear();
	carray.clear();

	//When voxels are adjacent but different height to each other from our
	//perspective, we represent the height difference by drawing black lines
	//between the voxels.
	for(const VoxelPair& p : voxels) {
		const VoxelPos pos = normalize_pos(p.first);

		const int x1 = x() + width()/2 + pos[0]*voxel_width_;
		const int y1 = y() + height()/2 + pos[1]*voxel_width_*invert_y_;

		const int x2 = x1 + voxel_width_;
		const int y2 = y1 - voxel_width_;

		VoxelPos actual_pos = get_editor().get_selected_voxel(p.first, facing_, vector_[facing_] < 0);
		if(actual_pos != p.first) {
			continue;
		}

		VoxelPos down = p.first;
		VoxelPos right = p.first;

		switch(facing_) {
			case 0: down[1]--; right[2]++; break;
			case 1: down[2]++; right[0]++; break;
			case 2: down[1]--; right[0]++; break;
		}

		if(get_editor().get_selected_voxel(down, facing_, vector_[facing_] < 0) != down) {
			varray.push_back(x1);
			varray.push_back(y1);
			varray.push_back(x2);
			varray.push_back(y1);

			carray.push_back(0);
			carray.push_back(0);
			carray.push_back(0);
			carray.push_back(1);
			carray.push_back(0);
			carray.push_back(0);
			carray.push_back(0);
			carray.push_back(1);
		}

		if(get_editor().get_selected_voxel(right, facing_, vector_[facing_] < 0) != right) {
			varray.push_back(x2);
			varray.push_back(y1);
			varray.push_back(x2);
			varray.push_back(y2);

			carray.push_back(0);
			carray.push_back(0);
			carray.push_back(0);
			carray.push_back(1);
			carray.push_back(0);
			carray.push_back(0);
			carray.push_back(0);
			carray.push_back(1);
		}
	}

	{
		glm::vec3 camera_vec = get_iso_renderer().camera().position();
		GLfloat camera_pos[2];
		GLfloat* camera_pos_ptr = camera_pos;
		int dimensions[3] = {0, 2, 1};
		for(int n = 0; n != 3; ++n) {
			if(dimensions[n] != facing_) {
				*camera_pos_ptr++ = camera_vec[dimensions[n]];
			}
		}

		varray.push_back(x() + width()/2);
		varray.push_back(y() + height()/2);
		varray.push_back(x() + width()/2 + camera_pos[0]*voxel_width_);
		varray.push_back(y() + height()/2 + camera_pos[1]*voxel_width_*invert_y_);

		carray.push_back(1);
		carray.push_back(0);
		carray.push_back(1);
		carray.push_back(0.5);
		carray.push_back(1);
		carray.push_back(0);
		carray.push_back(1);
		carray.push_back(0.5);
	}


	gles2::active_shader()->shader()->vertex_array(2, GL_FLOAT, 0, 0, &varray[0]);
	gles2::active_shader()->shader()->color_array(4, GL_FLOAT, 0, 0, &carray[0]);
	glDrawArrays(GL_LINES, 0, varray.size()/2);
}

class perspective_widget : public gui::dialog
{
public:
	perspective_widget(const rect& area, int xdir, int ydir, int zdir);
	void init();
private:
	void flip();
	int xdir_, ydir_, zdir_;
	bool flipped_;

	boost::intrusive_ptr<perspective_renderer> renderer_;
	gui::label_ptr description_label_;
};

perspective_widget::perspective_widget(const rect& area, int xdir, int ydir, int zdir)
  : dialog(area.x(), area.y(), area.w(), area.h()),
    xdir_(xdir), ydir_(ydir), zdir_(zdir), flipped_(false)
{
	init();
}

void perspective_widget::init()
{
	clear();

	renderer_.reset(new perspective_renderer(xdir_, ydir_, zdir_));

	grid_ptr toolbar(new grid(4));

	std::string description;
	if(xdir_) { description = flipped_ ? "Reverse" : "Side"; }
	else if(ydir_) { description = flipped_ ? "Bottom" : "Top"; }
	else if(zdir_) { description = flipped_ ? "Back" : "Front"; }

	description_label_.reset(new label(description, 12));
	toolbar->add_col(description_label_);
	toolbar->add_col(new button(new label("Flip", graphics::color("antique_white").as_sdl_color(), 14, "Montaga-Regular"), boost::bind(&perspective_widget::flip, this)));
	toolbar->add_col(new button(new label("+", graphics::color("antique_white").as_sdl_color(), 14, "Montaga-Regular"), boost::bind(&perspective_renderer::zoom_in, renderer_.get())));
	toolbar->add_col(new button(new label("-", graphics::color("antique_white").as_sdl_color(), 14, "Montaga-Regular"), boost::bind(&perspective_renderer::zoom_out, renderer_.get())));
	add_widget(toolbar);

	add_widget(renderer_);
	renderer_->set_dim(width(), height() - renderer_->y());
};

void perspective_widget::flip()
{
	flipped_ = !flipped_;
	xdir_ *= -1;
	ydir_ *= -1;
	zdir_ *= -1;
	init();
}

voxel_editor::voxel_editor(const rect& r, const std::string& fname)
  : dialog(r.x(), r.y(), r.w(), r.h()), area_(r),
    current_layer_(0), highlight_layer_(-1),
    fname_(fname), tool_(TOOL_PENCIL), symmetric_(false)
{
	if(fname_.empty()) {
		layers_.push_back(Layer());
	} else {
		variant doc = json::parse_from_file(fname_);
		model_ = read_model(doc);

		for(const LayerType& layer_type : model_.layer_types) {
			auto itor = layer_type.variations.find(layer_type.last_edited_variation);
			if(itor == layer_type.variations.end()) {
				itor = layer_type.variations.begin();
			}

			assert(itor != layer_type.variations.end());

			layers_.push_back(itor->second);
		}
	}

	g_voxel_editor = this;
	init();
	build_voxels();
}

voxel_editor::~voxel_editor()
{
	if(g_voxel_editor == this) {
		g_voxel_editor = NULL;
	}
}

void voxel_editor::init()
{
	clear();

	const int sidebar_padding = 200;
	const int between_padding = 10;
	const int widget_width = (area_.w() - sidebar_padding - between_padding)/2;
	const int widget_height = (area_.h() - between_padding)/2;
	widget_ptr w;

	w.reset(new perspective_widget(rect(area_.x(), area_.y(), widget_width, widget_height), 1, 0, 0));
	add_widget(w, w->x(), w->y());

	w.reset(new perspective_widget(rect(area_.x() + widget_width + between_padding, area_.y(), widget_width, widget_height), 0, 1, 0));
	add_widget(w, w->x(), w->y());

	w.reset(new perspective_widget(rect(area_.x(), area_.y() + widget_height + between_padding, widget_width, widget_height), 0, 0, 1));
	add_widget(w, w->x(), w->y());

	if(!iso_renderer_) {
		iso_renderer_.reset(new iso_renderer(rect(area_.x() + widget_width + between_padding, area_.y() + widget_height + between_padding, widget_width, widget_height)));
	}
	add_widget(iso_renderer_, iso_renderer_->x(), iso_renderer_->y());

	grid_ptr toolbar(new grid(3));

	toolbar->add_col(widget_ptr(new button(new label("Save", graphics::color("antique_white").as_sdl_color(), 14, "Montaga-Regular"), boost::bind(&voxel_editor::on_save, this))));
	toolbar->add_col(widget_ptr(new button(new label("Undo", graphics::color("antique_white").as_sdl_color(), 14, "Montaga-Regular"), boost::bind(&voxel_editor::undo, this))));
	toolbar->add_col(widget_ptr(new button(new label("Redo", graphics::color("antique_white").as_sdl_color(), 14, "Montaga-Regular"), boost::bind(&voxel_editor::redo, this))));
	add_widget(toolbar, area_.x2() - 190, area_.y() + 4);

	tool_borders_.clear();
	grid_ptr tools_grid(new grid(3));

	for(int n = 0; ToolIcons[n]; ++n) {
		assert(n < NUM_VOXEL_TOOLS);
		button_ptr tool_button(
		  new button(widget_ptr(new gui_section_widget(ToolIcons[n], 26, 26)),
		      boost::bind(&voxel_editor::select_tool, this, static_cast<VOXEL_TOOL>(n))));
		tool_borders_.push_back(new border_widget(tool_button, tool_ == n ? graphics::color_white() : graphics::color_black()));
		tools_grid->add_col(widget_ptr(tool_borders_.back()));
	}

	tools_grid->finish_row();

	add_widget(tools_grid);

	add_widget(widget_ptr(new checkbox(new label("Symmetric", graphics::color("antique_white").as_sdl_color(), 14, "Montaga-Regular"), symmetric_, boost::bind(&voxel_editor::set_symmetric, this, _1))));

	if(model_.layer_types.empty() == false) {
		assert(model_.layer_types.size() == layers_.size());
		grid_ptr layers_grid(new grid(1));

		for(int n = 0; n != layers_.size(); ++n) {
			layers_grid->add_col(widget_ptr(new label(model_.layer_types[n].name + ": " + layers_[n].name)));
		}

		layers_grid->allow_selection();
		layers_grid->set_draw_selection_highlight();
		layers_grid->set_default_selection(current_layer_);
		layers_grid->register_mouseover_callback(boost::bind(&voxel_editor::mouseover_layer, this, _1));
		layers_grid->register_selection_callback(boost::bind(&voxel_editor::select_layer, this, _1, layers_grid.get()));

		add_widget(layers_grid);
	}

	if(!color_picker_) {
		color_picker_.reset(new color_picker(rect(area_.x() + area_.w() - 190, area_.y() + 6, 180, 440)));
		color_picker_->set_primary_color(graphics::color(255, 0, 0));
	}
	add_widget(color_picker_);

	pos_label_.reset(new label("", 12));
	add_widget(pos_label_, area_.x() + area_.w() - pos_label_->width() - 100,
	                       area_.y() + area_.h() - pos_label_->height() - 30 );


}

void voxel_editor::set_voxel(const VoxelPos& pos, const Voxel& voxel)
{
	layer().map[pos] = voxel;
	if(symmetric_) {
		VoxelPos opposite_pos = pos;
		opposite_pos[0] = -1*opposite_pos[0] - 1;
		layer().map[opposite_pos] = voxel;
	}
	build_voxels();
}

void voxel_editor::delete_voxel(const VoxelPos& pos)
{
	layer().map.erase(pos);
	if(symmetric_) {
		VoxelPos opposite_pos = pos;
		opposite_pos[0] = -1*opposite_pos[0] - 1;
		layer().map.erase(opposite_pos);
	}
	build_voxels();
}

bool voxel_editor::set_cursor(const VoxelPos& pos)
{
	if(cursor_ && *cursor_ == pos) {
		return false;
	}

	cursor_.reset(new VoxelPos(pos));
	if(pos_label_) {
		pos_label_->set_text(formatter() << "(" << pos[0] << "," << pos[1] << "," << pos[2] << ")");
		pos_label_->set_loc(area_.x() + area_.w() - pos_label_->width() - 8,
		                    area_.y() + area_.h() - pos_label_->height() - 4);
	}

	return true;
}

VoxelPos voxel_editor::get_selected_voxel(const VoxelPos& pos, int facing, bool reverse)
{
	const int flip = reverse ? -1 : 1;
	VoxelPos result = pos;
	bool found = false;
	int best_value = 0;
	for(const VoxelPair& p : voxels_) {
		bool is_equal = true;
		for(int n = 0; n != 3; ++n) {
			if(n != facing && pos[n] != p.first[n]) {
				is_equal = false;
				break;
			}
		}

		if(!is_equal) {
			continue;
		}

		const int value = flip*p.first[facing];
		if(found == false || value >= best_value) {
			best_value = value;
			result = p.first;
			found = true;
		}
	}

	return result;
}

bool voxel_editor::handle_event(const SDL_Event& event, bool claimed)
{
	if(event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_RESIZED) {
		video_resize(event);
		set_dim(preferences::actual_screen_width(), preferences::actual_screen_height());
		init();
		return true;
	}
	return dialog::handle_event(event, claimed);
}

void voxel_editor::on_color_changed(const graphics::color& color)
{
}

void voxel_editor::select_tool(VOXEL_TOOL tool)
{
	tool_ = tool;
	init();
}

void voxel_editor::set_symmetric(bool value)
{
	const bool old_value = symmetric_;
	symmetric_ = value;
	get_editor().execute_command(
		[this, value]() { this->symmetric_ = value; },
		[this, old_value]() { this->symmetric_ = old_value; }
	);
}

void voxel_editor::mouseover_layer(int nlayer)
{
	highlight_layer_ = nlayer;
}

void voxel_editor::select_layer(int nlayer, grid* layer_grid)
{
	std::cerr << "SELECT LAYER: " << nlayer << "\n";
	if(nlayer != -1) {
		assert(nlayer >= 0 && nlayer < layers_.size());
		const int old_layer = current_layer_;

		execute_command(
			[this, nlayer]() { this->current_layer_ = nlayer; },
			[this, old_layer]() { this->current_layer_ = old_layer; }
		);
	} else {
		layer_grid->set_default_selection(current_layer_);
	}
}

void voxel_editor::on_save()
{
	if(fname_.empty()) {
		std::cerr << "NO FILENAME. CANNOT SAVE\n";
		return;
	}

	assert(layers_.size() == model_.layer_types.size());

	for(int n = 0; n != layers_.size(); ++n) {
		model_.layer_types[n].variations[layers_[n].name] = layers_[n];
		model_.layer_types[n].last_edited_variation = layers_[n].name;
	}

	variant doc = write_model(model_);
	sys::write_file(fname_, doc.write_json());
}

void voxel_editor::undo()
{
	if(undo_.empty() == false) {
		Command cmd = undo_.back();
		undo_.pop_back();
		cmd.undo();
		redo_.push_back(cmd);
		init();
	}
}

void voxel_editor::redo()
{
	if(redo_.empty() == false) {
		Command cmd = redo_.back();
		redo_.pop_back();
		cmd.redo();
		undo_.push_back(cmd);
		init();
	}
}

void voxel_editor::handle_process()
{
	VOXEL_TOOL current_tool = tool();
	for(int n = 0; n != tool_borders_.size(); ++n) {
		tool_borders_[n]->set_color(n == current_tool ? graphics::color_white() : graphics::color_black());
	}

	dialog::handle_process();
}

void voxel_editor::execute_command(std::function<void()> redo, std::function<void()> undo)
{
	execute_command(Command(redo, undo));
}

void voxel_editor::execute_command(const Command& cmd)
{
	cmd.redo();
	undo_.push_back(cmd);
	redo_.clear();
}

void voxel_editor::build_voxels()
{
	voxels_.clear();
	int nlayer = 0;
	for(const Layer& layer : layers_) {
		for(VoxelPair p : layer.map) {
			p.second.nlayer = nlayer;
			voxels_.insert(p);
		}

		++nlayer;
	}
}

}

UTILITY(voxel_editor)
{
	std::deque<std::string> arguments(args.begin(), args.end());

	ASSERT_LOG(arguments.size() <= 1, "Unexpected arguments");

	std::string fname;
	if(arguments.empty() == false) {
		fname = module::map_file(arguments.front());
	}
	
	boost::intrusive_ptr<voxel_editor> editor(new voxel_editor(rect(0, 0, preferences::actual_screen_width(), preferences::actual_screen_height()), fname));
	editor->show_modal();
}

#endif //USE_GLES2