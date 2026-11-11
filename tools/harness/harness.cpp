// glyphnotic-harness: runs Glyphnotic filters on a still image in a headless
// libobs, without the OBS frontend. It can write:
//
//   - the filter's grid dump (the debug build's Dump grid button);
//   - one output frame as OBS outputs it (8-bit canvas only; BGRA from OBS,
//     saved as RGBA PAM);
//   - OBS's main canvas texture (any canvas format; raw, see below);
//   - lagged-frame counts and OBS's per-source GPU times over a run;
//   - the filter's settings and properties after changes made the way the
//     properties window makes them;
//   - a per-frame recording of the grid (held and shown glyphs, boils,
//     colors), and two consecutive output frames.
//
// Besides OBS's image source it has two test sources of its own: an HDR
// still, and a frame sequence that plays stills in turn and can add fresh
// Gaussian noise to every frame.
//
// It's a test tool and is never shipped. It needs a plugin built with
// GLYPHNOTIC_DEBUG_TOOLS=ON and an installed OBS of the same version that
// the plugin was built against. See tools/README.md for the options.

#include <obs.h>
#include <obs-module.h>
#include <graphics/graphics.h>
#include <util/base.h>
#include <util/bmem.h>
#include <util/platform.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <sstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

// OBS's source profiler (libobs/util/source-profiler.h, OBS 33.0.1). libobs
// exports it but doesn't install the header, so it's declared here.
extern "C" {
typedef struct profiler_result {
	uint64_t tick_avg;
	uint64_t tick_max;
	uint64_t render_avg;
	uint64_t render_max;
	uint64_t render_gpu_avg;
	uint64_t render_gpu_max;
	uint64_t render_sum;
	uint64_t render_gpu_sum;
	double async_input;
	double async_rendered;
	uint64_t async_input_best;
	uint64_t async_input_worst;
	uint64_t async_rendered_best;
	uint64_t async_rendered_worst;
} profiler_result_t;
EXPORT void source_profiler_enable(bool enable);
EXPORT void source_profiler_gpu_enable(bool enable);
EXPORT bool source_profiler_fill_result(obs_source_t *source, profiler_result_t *result);
}

namespace {

const char *kUsage =
	"usage: glyphnotic-harness --obs <dir> --plugin <module> --plugin-data <dir>\n"
	"         (--image <png> | --hdr-image <png> [--hdr-scale <x>])\n"
	"         [--filter <id> [--settings <json>]]...  [--instances <n>] [--on-scene]\n"
	"         [--renderer d3d11|opengl] [--format bgra|i010|p010] [--colorspace srgb|709|2100pq|2100hlg]\n"
	"         [--range full|partial] [--fps <n>] [--frames <n>] [--profile] [--verbose]\n"
	"         [--dump <json>] [--frame <pam>] [--main-texture <raw>] [--check-visible <property>]\n"
	"         [--ui-set <json> [--ui-no-update]] [--press <button>]... [--wait-frames <n>]\n"
	"         [--settings-out <json>] [--properties-out <json>]\n"
	"         [--then-image <png> --then-frames <n> --dump2 <json>]\n"
	"         [--also <png>]... [--period <frames>] [--noise <sigma>] [--noise-seed <n>]\n"
	"         [--record <bin> --record-frames <n> [--record-colors]] [--frame-pair <prefix>]\n";

struct FilterSpec {
	std::string id;
	std::string settings = "{}";
};

struct Args {
	std::string obs_dir, plugin, plugin_data, image, dump, frame, main_texture;
	std::string check_visible;      // a property of the first Glyphnotic filter
	std::string ui_set;             // settings changed as the properties window would
	bool ui_no_update = false;      // leave out the update after --ui-set: test the callbacks alone
	std::vector<std::string> press; // buttons of the first Glyphnotic filter
	int wait_frames = 10;           // frames to wait after --ui-set / --press
	std::string settings_out, properties_out;
	std::string then_image, dump2; // switch the still, run, dump again
	int then_frames = 0;
	bool hdr_source = false;
	double hdr_scale = 1.0;
	// The frame sequence source: --image, then each --also, `period` frames
	// each (0: the first only), with fresh Gaussian noise of `noise` (8-bit
	// units) on every frame.
	std::vector<std::string> also;
	int period = 0;
	double noise = 0.0;
	int noise_seed = 1;
	bool frames_source = false;
	std::string record; // per-frame recording of the first Glyphnotic filter
	int record_frames = 0;
	bool record_colors = false;
	std::string frame_pair; // two consecutive output frames: <prefix>-0.pam, -1.pam
	std::vector<FilterSpec> filters;
	int instances = 1;
	uint32_t canvas_w = 0, canvas_h = 0; // 0 = the still's size, rounded up to a multiple of 4
	float pos_x = 0.0f, pos_y = 0.0f;    // where the still sits in a scene
	bool on_scene = false;
	std::string renderer; // "d3d11" or "opengl"; empty = the platform default
	std::string format = "bgra", colorspace = "srgb", range = "full";
	int fps = 30;
	int frames = 0; // frames to run after setup, for lag and timing
	bool profile = false;
	bool verbose = false;
};

bool g_verbose = false;

void log_handler(int level, const char *format, va_list args, void *param)
{
	(void)param;
	if (level > LOG_WARNING && !g_verbose) {
		// Keep Glyphnotic's own messages; drop libobs's chatter.
		if (strncmp(format, "[glyphnotic]", 12) != 0)
			return;
	}
	vfprintf(stderr, format, args);
	fputc('\n', stderr);
}

bool parse_args(int argc, char **argv, Args &a)
{
	for (int i = 1; i < argc; i++) {
		std::string k = argv[i];
		std::string v;
		auto next = [&](std::string &out) {
			if (i + 1 >= argc)
				return false;
			out = argv[++i];
			return true;
		};
		bool ok = true;
		if (k == "--obs")
			ok = next(a.obs_dir);
		else if (k == "--plugin")
			ok = next(a.plugin);
		else if (k == "--plugin-data")
			ok = next(a.plugin_data);
		else if (k == "--image")
			ok = next(a.image);
		else if (k == "--hdr-image") {
			ok = next(a.image);
			a.hdr_source = true;
		} else if (k == "--hdr-scale" && (ok = next(v)))
			a.hdr_scale = atof(v.c_str());
		else if (k == "--filter" && (ok = next(v)))
			a.filters.push_back({v});
		else if (k == "--settings" && !a.filters.empty())
			ok = next(a.filters.back().settings);
		else if (k == "--canvas" && (ok = next(v)))
			ok = sscanf(v.c_str(), "%ux%u", &a.canvas_w, &a.canvas_h) == 2;
		else if (k == "--pos" && (ok = next(v)))
			ok = sscanf(v.c_str(), "%f,%f", &a.pos_x, &a.pos_y) == 2;
		else if (k == "--instances" && (ok = next(v)))
			a.instances = atoi(v.c_str());
		else if (k == "--dump")
			ok = next(a.dump);
		else if (k == "--frame")
			ok = next(a.frame);
		else if (k == "--main-texture")
			ok = next(a.main_texture);
		else if (k == "--check-visible")
			ok = next(a.check_visible);
		else if (k == "--ui-set")
			ok = next(a.ui_set);
		else if (k == "--ui-no-update")
			a.ui_no_update = true;
		else if (k == "--press" && (ok = next(v)))
			a.press.push_back(v);
		else if (k == "--wait-frames" && (ok = next(v)))
			a.wait_frames = atoi(v.c_str());
		else if (k == "--settings-out")
			ok = next(a.settings_out);
		else if (k == "--properties-out")
			ok = next(a.properties_out);
		else if (k == "--then-image")
			ok = next(a.then_image);
		else if (k == "--then-frames" && (ok = next(v)))
			a.then_frames = atoi(v.c_str());
		else if (k == "--dump2")
			ok = next(a.dump2);
		else if (k == "--also" && (ok = next(v))) {
			a.also.push_back(v);
			a.frames_source = true;
		} else if (k == "--period" && (ok = next(v))) {
			a.period = atoi(v.c_str());
			a.frames_source = true;
		} else if (k == "--noise" && (ok = next(v))) {
			a.noise = atof(v.c_str());
			a.frames_source = true;
		} else if (k == "--noise-seed" && (ok = next(v)))
			a.noise_seed = atoi(v.c_str());
		else if (k == "--frame-sequence")
			a.frames_source = true;
		else if (k == "--record")
			ok = next(a.record);
		else if (k == "--record-frames" && (ok = next(v)))
			a.record_frames = atoi(v.c_str());
		else if (k == "--record-colors")
			a.record_colors = true;
		else if (k == "--frame-pair")
			ok = next(a.frame_pair);
		else if (k == "--renderer")
			ok = next(a.renderer);
		else if (k == "--format")
			ok = next(a.format);
		else if (k == "--colorspace")
			ok = next(a.colorspace);
		else if (k == "--range")
			ok = next(a.range);
		else if (k == "--fps" && (ok = next(v)))
			a.fps = atoi(v.c_str());
		else if (k == "--frames" && (ok = next(v)))
			a.frames = atoi(v.c_str());
		else if (k == "--on-scene")
			a.on_scene = true;
		else if (k == "--profile")
			a.profile = true;
		else if (k == "--verbose")
			a.verbose = true;
		else
			ok = false;
		if (!ok) {
			fprintf(stderr, "bad argument: %s\n", k.c_str());
			return false;
		}
	}
	return !a.obs_dir.empty() && !a.plugin.empty() && !a.plugin_data.empty() && !a.image.empty() &&
	       a.instances >= 1 && a.fps >= 1;
}

// Width and height from a PNG's IHDR chunk.
bool png_size(const std::string &path, uint32_t &w, uint32_t &h)
{
	std::ifstream f(path, std::ios::binary);
	unsigned char b[24];
	if (!f.read(reinterpret_cast<char *>(b), sizeof(b)) || memcmp(b + 12, "IHDR", 4) != 0)
		return false;
	w = (uint32_t(b[16]) << 24) | (uint32_t(b[17]) << 16) | (uint32_t(b[18]) << 8) | b[19];
	h = (uint32_t(b[20]) << 24) | (uint32_t(b[21]) << 16) | (uint32_t(b[22]) << 8) | b[23];
	return w > 0 && h > 0;
}

// ---------------------------------------------------------------------------
// A test-only HDR source: an 8-bit PNG decoded to linear sRGB, multiplied by
// a scale, uploaded as a half-float texture and reported as GS_CS_709_EXTENDED
// (linear, 1.0 = SDR white). OBS converts it like any HDR source.

uint16_t float_to_half(float f)
{
	uint32_t x;
	memcpy(&x, &f, 4);
	const uint32_t sign = (x >> 16) & 0x8000;
	int exp = int((x >> 23) & 0xFF) - 127 + 15;
	uint32_t mant = x & 0x7FFFFF;
	if (exp <= 0) {
		if (exp < -10)
			return uint16_t(sign);
		mant |= 0x800000;
		const int shift = 14 - exp;
		uint32_t h = mant >> shift;
		if ((mant >> (shift - 1)) & 1)
			h++; // round half up
		return uint16_t(sign | h);
	}
	if (exp >= 31)
		return uint16_t(sign | 0x7C00);
	uint32_t h = sign | (uint32_t(exp) << 10) | (mant >> 13);
	if (mant & 0x1000)
		h++; // round half up
	return uint16_t(h);
}

struct HdrSource {
	gs_texture_t *tex = nullptr;
	uint32_t w = 0, h = 0;
};

const char *hdr_get_name(void *)
{
	return "Harness HDR still";
}

void *hdr_create(obs_data_t *settings, obs_source_t *)
{
	auto *s = new HdrSource();
	const char *file = obs_data_get_string(settings, "file");
	const double scale = obs_data_get_double(settings, "scale");
	enum gs_color_format fmt = GS_UNKNOWN;
	uint8_t *px = gs_create_texture_file_data(file, &fmt, &s->w, &s->h);
	if (!px)
		return s;
	const bool bgr = fmt == GS_BGRA || fmt == GS_BGRX;
	std::vector<uint16_t> half(size_t(s->w) * s->h * 4);
	for (size_t i = 0; i < size_t(s->w) * s->h; i++) {
		for (int c = 0; c < 3; c++) {
			const int src = bgr ? 2 - c : c;
			const double u = px[i * 4 + src] / 255.0;
			const double lin = u <= 0.04045 ? u / 12.92 : std::pow((u + 0.055) / 1.055, 2.4);
			half[i * 4 + c] = float_to_half(float(lin * scale));
		}
		half[i * 4 + 3] = float_to_half(1.0f);
	}
	bfree(px);
	const uint8_t *levels[] = {reinterpret_cast<const uint8_t *>(half.data())};
	obs_enter_graphics();
	s->tex = gs_texture_create(s->w, s->h, GS_RGBA16F, 1, levels, 0);
	obs_leave_graphics();
	return s;
}

void hdr_destroy(void *data)
{
	auto *s = static_cast<HdrSource *>(data);
	obs_enter_graphics();
	gs_texture_destroy(s->tex);
	obs_leave_graphics();
	delete s;
}

uint32_t hdr_width(void *data)
{
	return static_cast<HdrSource *>(data)->w;
}

uint32_t hdr_height(void *data)
{
	return static_cast<HdrSource *>(data)->h;
}

void hdr_render(void *data, gs_effect_t *)
{
	auto *s = static_cast<HdrSource *>(data);
	if (!s->tex)
		return;
	gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	gs_effect_set_texture(gs_effect_get_param_by_name(effect, "image"), s->tex);
	while (gs_effect_loop(effect, "Draw"))
		gs_draw_sprite(s->tex, 0, s->w, s->h);
}

enum gs_color_space hdr_color_space(void *, size_t, const enum gs_color_space *)
{
	return GS_CS_709_EXTENDED;
}

void register_hdr_source()
{
	obs_source_info info = {};
	info.id = "harness_hdr_still";
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_SRGB | OBS_SOURCE_CUSTOM_DRAW;
	info.get_name = hdr_get_name;
	info.create = hdr_create;
	info.destroy = hdr_destroy;
	info.get_width = hdr_width;
	info.get_height = hdr_height;
	info.video_render = hdr_render;
	info.video_get_color_space = hdr_color_space;
	obs_register_source(&info);
}

// ---------------------------------------------------------------------------
// A test-only frame sequence: 8-bit stills shown in turn, `period` frames
// each, optionally with fresh Gaussian noise on every frame. Noise is added
// to each color channel of the straight 8-bit values and rounded, as
// round(v + n) = v + round(n) with n ~ N(0, sigma^2), drawn from a table of
// round(n)'s distribution; the result is clipped to 0..255 and premultiplied
// by alpha as OBS's image loader stores it (round(c * a / 255)). It draws its
// texture with OBS's default effect, without OBS_SOURCE_SRGB, so the bytes
// reach a filter unchanged (run_motion.py checks that a filter sees the same
// grid as with OBS's image source).

struct FramesSource {
	std::vector<std::vector<uint8_t>> images; // straight RGBA
	uint32_t w = 0, h = 0;
	int period = 0;
	double noise = 0.0;
	uint64_t seed = 1;
	std::vector<int8_t> noise_table; // 65536 draws of round(n)
	std::vector<uint8_t> frame;      // premultiplied RGBA, uploaded
	gs_texture_t *tex = nullptr;
	uint64_t ticks = 0;
	uint64_t uploaded = UINT64_MAX;
	size_t uploaded_image = SIZE_MAX;
};

uint64_t splitmix64(uint64_t &state)
{
	uint64_t z = (state += 0x9E3779B97F4A7C15ull);
	z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
	z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
	return z ^ (z >> 31);
}

// round(n) for n ~ N(0, sigma^2), as a table indexed by 16 random bits.
std::vector<int8_t> make_noise_table(double sigma)
{
	std::vector<int8_t> table(65536, 0);
	if (sigma <= 0.0)
		return table;
	auto cdf = [&](double x) {
		return 0.5 * std::erfc(-x / (sigma * std::sqrt(2.0)));
	};
	int k = -127;
	for (int u = 0; u < 65536; u++) {
		const double x = (u + 0.5) / 65536.0;
		while (k < 127 && cdf(k + 0.5) <= x)
			k++;
		table[size_t(u)] = int8_t(k);
	}
	return table;
}

const char *frames_get_name(void *)
{
	return "Harness frame sequence";
}

void *frames_create(obs_data_t *settings, obs_source_t *)
{
	auto *s = new FramesSource();
	std::istringstream files(obs_data_get_string(settings, "files"));
	std::string file;
	while (std::getline(files, file)) {
		enum gs_color_format fmt = GS_UNKNOWN;
		uint32_t w = 0, h = 0;
		uint8_t *px = gs_create_texture_file_data(file.c_str(), &fmt, &w, &h);
		if (!px || (!s->images.empty() && (w != s->w || h != s->h))) {
			fprintf(stderr, "frame sequence: cannot use %s\n", file.c_str());
			bfree(px);
			s->images.clear();
			break;
		}
		s->w = w;
		s->h = h;
		const bool bgr = fmt == GS_BGRA || fmt == GS_BGRX;
		const bool opaque = fmt == GS_BGRX;
		std::vector<uint8_t> rgba(size_t(w) * h * 4);
		for (size_t i = 0; i < size_t(w) * h; i++) {
			for (int c = 0; c < 3; c++)
				rgba[i * 4 + c] = px[i * 4 + (bgr ? 2 - c : c)];
			rgba[i * 4 + 3] = opaque ? 255 : px[i * 4 + 3];
		}
		bfree(px);
		s->images.push_back(std::move(rgba));
	}
	s->period = int(obs_data_get_int(settings, "period"));
	s->noise = obs_data_get_double(settings, "noise");
	s->seed = uint64_t(obs_data_get_int(settings, "seed"));
	s->noise_table = make_noise_table(s->noise);
	if (!s->images.empty()) {
		s->frame.resize(s->images[0].size());
		obs_enter_graphics();
		s->tex = gs_texture_create(s->w, s->h, GS_RGBA, 1, nullptr, GS_DYNAMIC);
		obs_leave_graphics();
	}
	return s;
}

void frames_destroy(void *data)
{
	auto *s = static_cast<FramesSource *>(data);
	obs_enter_graphics();
	gs_texture_destroy(s->tex);
	obs_leave_graphics();
	delete s;
}

uint32_t frames_width(void *data)
{
	return static_cast<FramesSource *>(data)->w;
}

uint32_t frames_height(void *data)
{
	return static_cast<FramesSource *>(data)->h;
}

void frames_tick(void *data, float)
{
	static_cast<FramesSource *>(data)->ticks++;
}

// Builds and uploads this tick's frame (render thread, graphics context entered).
void frames_update(FramesSource *s)
{
	const size_t index = s->period > 0 ? size_t(s->ticks / uint64_t(s->period)) % s->images.size() : 0;
	if (s->uploaded == s->ticks || (s->noise <= 0.0 && index == s->uploaded_image))
		return;
	const std::vector<uint8_t> &img = s->images[index];
	uint64_t state = s->seed * 0x100000001B3ull + s->ticks;
	uint64_t bits = 0;
	int left = 0;
	for (size_t i = 0; i < img.size(); i += 4) {
		const double a = img[i + 3];
		for (int c = 0; c < 3; c++) {
			int v = img[i + size_t(c)];
			if (s->noise > 0.0) {
				if (left == 0) {
					bits = splitmix64(state);
					left = 4;
				}
				v += s->noise_table[size_t(bits & 0xFFFF)];
				bits >>= 16;
				left--;
				v = v < 0 ? 0 : v > 255 ? 255 : v;
			}
			s->frame[i + size_t(c)] = uint8_t(std::floor(v * (a / 255.0) + 0.5));
		}
		s->frame[i + 3] = img[i + 3];
	}
	gs_texture_set_image(s->tex, s->frame.data(), s->w * 4, false);
	s->uploaded = s->ticks;
	s->uploaded_image = index;
}

void frames_render(void *data, gs_effect_t *)
{
	auto *s = static_cast<FramesSource *>(data);
	if (!s->tex)
		return;
	frames_update(s);
	const bool prev_srgb = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(false);
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
	gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	gs_effect_set_texture(gs_effect_get_param_by_name(effect, "image"), s->tex);
	while (gs_effect_loop(effect, "Draw"))
		gs_draw_sprite(s->tex, 0, s->w, s->h);
	gs_blend_state_pop();
	gs_enable_framebuffer_srgb(prev_srgb);
}

void register_frames_source()
{
	obs_source_info info = {};
	info.id = "harness_frames";
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW;
	info.get_name = frames_get_name;
	info.create = frames_create;
	info.destroy = frames_destroy;
	info.get_width = frames_width;
	info.get_height = frames_height;
	info.video_tick = frames_tick;
	info.video_render = frames_render;
	obs_register_source(&info);
}

// ---------------------------------------------------------------------------

struct FrameGrab {
	std::mutex mutex;
	std::vector<uint8_t> rgba;
	uint32_t width = 0, height = 0;
	bool bgra = true; // only BGRA frames are kept
	std::atomic<int> count{0};
	int pair_left = 0; // frames still to keep for --frame-pair
	std::vector<std::vector<uint8_t>> pair;
};

void raw_video(void *param, struct video_data *frame)
{
	auto *g = static_cast<FrameGrab *>(param);
	if (g->bgra) {
		std::lock_guard<std::mutex> lock(g->mutex);
		g->rgba.resize(size_t(g->width) * g->height * 4);
		for (uint32_t y = 0; y < g->height; y++) {
			const uint8_t *src = frame->data[0] + size_t(y) * frame->linesize[0];
			uint8_t *dst = g->rgba.data() + size_t(y) * g->width * 4;
			for (uint32_t x = 0; x < g->width; x++) {
				dst[x * 4 + 0] = src[x * 4 + 2];
				dst[x * 4 + 1] = src[x * 4 + 1];
				dst[x * 4 + 2] = src[x * 4 + 0];
				dst[x * 4 + 3] = src[x * 4 + 3];
			}
		}
		if (g->pair_left > 0) {
			g->pair.push_back(g->rgba);
			g->pair_left--;
		}
	}
	g->count++;
}

bool wait_for(const std::function<bool()> &done, int timeout_ms)
{
	const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
	while (!done()) {
		if (std::chrono::steady_clock::now() > end)
			return false;
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	return true;
}

bool load_module(const std::string &bin, const std::string &data)
{
	obs_module_t *module = nullptr;
	const int r = obs_open_module(&module, bin.c_str(), data.c_str());
	if (r != MODULE_SUCCESS) {
		fprintf(stderr, "cannot open module %s (error %d)\n", bin.c_str(), r);
		return false;
	}
	if (!obs_init_module(module)) {
		fprintf(stderr, "module %s failed to initialize\n", bin.c_str());
		return false;
	}
	return true;
}

// An OBS module's binary and data folder in an installed OBS.
void core_module(const std::string &obs_dir, const char *name, std::string &bin, std::string &data)
{
#ifdef _WIN32
	// OBS 33 keeps its own plugins in core/<name>/ with data in core/<name>/data/;
	// older versions in obs-plugins/64bit/ and data/obs-plugins/<name>/.
	bin = obs_dir + "/core/" + name + "/" + name + ".dll";
	data = obs_dir + "/core/" + name + "/data";
	if (!os_file_exists(bin.c_str())) {
		bin = obs_dir + "/obs-plugins/64bit/" + name + ".dll";
		data = obs_dir + "/data/obs-plugins/" + name;
	}
#else
	bin = obs_dir + "/lib/x86_64-linux-gnu/obs-plugins/" + name + ".so";
	data = obs_dir + "/share/obs/obs-plugins/" + name;
#endif
}

bool parse_video(const Args &a, obs_video_info &ovi)
{
	if (a.format == "bgra")
		ovi.output_format = VIDEO_FORMAT_BGRA;
	else if (a.format == "i010")
		ovi.output_format = VIDEO_FORMAT_I010;
	else if (a.format == "p010")
		ovi.output_format = VIDEO_FORMAT_P010;
	else
		return false;
	if (a.colorspace == "srgb")
		ovi.colorspace = VIDEO_CS_SRGB;
	else if (a.colorspace == "709")
		ovi.colorspace = VIDEO_CS_709;
	else if (a.colorspace == "2100pq")
		ovi.colorspace = VIDEO_CS_2100_PQ;
	else if (a.colorspace == "2100hlg")
		ovi.colorspace = VIDEO_CS_2100_HLG;
	else
		return false;
	if (a.range == "full")
		ovi.range = VIDEO_RANGE_FULL;
	else if (a.range == "partial")
		ovi.range = VIDEO_RANGE_PARTIAL;
	else
		return false;
	return true;
}

const char *format_name(enum gs_color_format f)
{
	switch (f) {
	case GS_BGRA:
		return "bgra8";
	case GS_RGBA:
		return "rgba8";
	case GS_RGBA16F:
		return "rgba16f";
	case GS_RGBA32F:
		return "rgba32f";
	default:
		return nullptr;
	}
}

// Saves OBS's main canvas texture: a text line "GHRAW <w> <h> <format>\n",
// then rows of texels with no padding.
bool save_main_texture(const std::string &path)
{
	bool ok = false;
	obs_enter_graphics();
	gs_texture_t *tex = obs_get_main_texture();
	if (tex) {
		const enum gs_color_format fmt = gs_texture_get_color_format(tex);
		const uint32_t w = gs_texture_get_width(tex), h = gs_texture_get_height(tex);
		const char *name = format_name(fmt);
		const uint32_t bpp = fmt == GS_RGBA16F ? 8 : fmt == GS_RGBA32F ? 16 : 4;
		gs_stagesurf_t *stage = name ? gs_stagesurface_create(w, h, fmt) : nullptr;
		if (stage) {
			gs_stage_texture(stage, tex);
			uint8_t *ptr = nullptr;
			uint32_t linesize = 0;
			if (gs_stagesurface_map(stage, &ptr, &linesize)) {
				FILE *f = os_fopen(path.c_str(), "wb");
				if (f) {
					fprintf(f, "GHRAW %u %u %s\n", w, h, name);
					for (uint32_t y = 0; y < h; y++)
						fwrite(ptr + size_t(y) * linesize, 1, size_t(w) * bpp, f);
					fclose(f);
					ok = true;
				}
				gs_stagesurface_unmap(stage);
			}
			gs_stagesurface_destroy(stage);
		} else {
			fprintf(stderr, "main texture format %d not supported\n", int(fmt));
		}
	}
	obs_leave_graphics();
	return ok;
}

void print_profile(const char *what, obs_source_t *source)
{
	profiler_result_t r = {};
	if (source && source_profiler_fill_result(source, &r))
		fprintf(stderr, "gpu: %s '%s': avg %.3f ms, max %.3f ms (render CPU avg %.3f ms)\n", what,
			obs_source_get_name(source), r.render_gpu_avg / 1e6, r.render_gpu_max / 1e6,
			r.render_avg / 1e6);
}

// Writes a string as a JSON string literal.
std::string json_str(const char *s)
{
	std::string out = "\"";
	for (const char *p = s ? s : ""; *p; p++) {
		if (*p == '"' || *p == '\\')
			out += '\\';
		out += *p;
	}
	return out + "\"";
}

// The properties as JSON: name, type, visibility and list items, groups nested.
void properties_json(obs_properties_t *props, std::string &out)
{
	out += "[";
	bool first = true;
	for (obs_property_t *p = obs_properties_first(props); p; obs_property_next(&p)) {
		if (!first)
			out += ",";
		first = false;
		out += "{\"name\":" + json_str(obs_property_name(p)) +
		       ",\"type\":" + std::to_string(int(obs_property_get_type(p))) +
		       ",\"visible\":" + (obs_property_visible(p) ? "true" : "false");
		if (obs_property_get_type(p) == OBS_PROPERTY_LIST) {
			out += ",\"items\":[";
			const bool strings = obs_property_list_format(p) == OBS_COMBO_FORMAT_STRING;
			for (size_t i = 0; i < obs_property_list_item_count(p); i++) {
				if (i)
					out += ",";
				out += strings ? json_str(obs_property_list_item_string(p, i))
					       : std::to_string(obs_property_list_item_int(p, i));
			}
			out += "]";
		}
		if (obs_property_get_type(p) == OBS_PROPERTY_GROUP) {
			out += ",\"content\":";
			properties_json(obs_property_group_content(p), out);
		}
		out += "}";
	}
	out += "]";
}

bool write_file(const std::string &path, const std::string &text)
{
	FILE *f = os_fopen(path.c_str(), "wb");
	if (!f)
		return false;
	fwrite(text.data(), 1, text.size(), f);
	fclose(f);
	return true;
}

bool write_pam(const std::string &path, const std::vector<uint8_t> &rgba, uint32_t w, uint32_t h)
{
	FILE *f = os_fopen(path.c_str(), "wb");
	if (!f)
		return false;
	fprintf(f, "P7\nWIDTH %u\nHEIGHT %u\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n", w, h);
	fwrite(rgba.data(), 1, rgba.size(), f);
	fclose(f);
	return true;
}

void set_env(const char *name, const std::string &value)
{
#ifdef _WIN32
	_putenv_s(name, value.c_str());
#else
	setenv(name, value.c_str(), 1);
#endif
}

// Presses a button of a filter.
bool press_button(obs_source_t *filter, const char *name)
{
	obs_properties_t *props = filter ? obs_source_properties(filter) : nullptr;
	obs_property_t *button = props ? obs_properties_get(props, name) : nullptr;
	if (button)
		obs_property_button_clicked(button, filter);
	else
		fprintf(stderr, "no %s button: needs a Glyphnotic filter from a build with GLYPHNOTIC_DEBUG_TOOLS=ON\n",
			name);
	obs_properties_destroy(props);
	return button != nullptr;
}

// Presses Record grid on a filter and waits for the recording.
bool press_record(obs_source_t *filter, const Args &a)
{
	os_unlink(a.record.c_str());
	set_env("GLYPHNOTIC_RECORD_PATH", a.record);
	set_env("GLYPHNOTIC_RECORD_FRAMES", std::to_string(a.record_frames));
	set_env("GLYPHNOTIC_RECORD_COLORS", a.record_colors ? "1" : "0");
	if (!press_button(filter, "record_grid"))
		return false;
	const int timeout = int(1000.0 * a.record_frames / a.fps * 4) + 30000;
	if (!wait_for([&] { return os_file_exists(a.record.c_str()); }, timeout)) {
		fprintf(stderr, "no recording written\n");
		return false;
	}
	return true;
}

// Presses Dump grid on a filter and waits for the file.
bool press_dump(obs_source_t *filter, const std::string &path)
{
	os_unlink(path.c_str());
	// The plugin writes its dump to GLYPHNOTIC_DUMP_PATH.
	set_env("GLYPHNOTIC_DUMP_PATH", path);
	obs_properties_t *props = filter ? obs_source_properties(filter) : nullptr;
	obs_property_t *button = props ? obs_properties_get(props, "dump_grid") : nullptr;
	if (!button) {
		fprintf(stderr, "no dump_grid button: needs a Glyphnotic filter from a build with "
				"GLYPHNOTIC_DEBUG_TOOLS=ON\n");
		obs_properties_destroy(props);
		return false;
	}
	obs_property_button_clicked(button, filter);
	obs_properties_destroy(props);
	if (!wait_for([&] { return os_file_exists(path.c_str()); }, 30000)) {
		fprintf(stderr, "no grid dump written\n");
		return false;
	}
	return true;
}

int run(const Args &a)
{
	uint32_t w = 0, h = 0;
	if (!png_size(a.image, w, h)) {
		fprintf(stderr, "cannot read PNG size of %s\n", a.image.c_str());
		return 2;
	}

#ifdef _WIN32
	// libobs loads its graphics module with LOAD_LIBRARY_SEARCH_DEFAULT_DIRS
	// and finds its data at ../../data/libobs relative to the working
	// directory, so point both at the OBS install.
	std::string bin_dir = a.obs_dir + "/bin/64bit";
	for (char &c : bin_dir)
		if (c == '/')
			c = '\\';
	std::wstring wbin(bin_dir.begin(), bin_dir.end());
	AddDllDirectory(wbin.c_str());
	SetCurrentDirectoryW(wbin.c_str());
	// --renderer opengl runs OBS's OpenGL renderer on Windows, which checks the
	// effects' GLSL translation without a Linux machine.
	const std::string graphics = a.renderer == "opengl" ? "libobs-opengl" : "libobs-d3d11";
#else
	const std::string graphics = "libobs-opengl";
#endif

	if (!obs_startup("en-US", nullptr, nullptr)) {
		fprintf(stderr, "obs_startup failed\n");
		return 2;
	}
	if (a.profile) {
		source_profiler_enable(true);
		source_profiler_gpu_enable(true);
	}

	struct obs_video_info ovi = {};
	ovi.graphics_module = graphics.c_str();
	ovi.fps_num = uint32_t(a.fps);
	ovi.fps_den = 1;
	// OBS's video output crashes on odd sizes, so the canvas is rounded up to a
	// multiple of 4; the still sits at the top-left and the frame is cropped back.
	ovi.base_width = ovi.output_width = a.canvas_w ? a.canvas_w : (w + 3) & ~3u;
	ovi.base_height = ovi.output_height = a.canvas_h ? a.canvas_h : (h + 3) & ~3u;
	ovi.adapter = 0;
	ovi.gpu_conversion = true;
	ovi.scale_type = OBS_SCALE_POINT;
	if (!parse_video(a, ovi)) {
		fprintf(stderr, "bad --format, --colorspace or --range\n");
		obs_shutdown();
		return 2;
	}
	const int vr = obs_reset_video(&ovi);
	if (vr != OBS_VIDEO_SUCCESS) {
		fprintf(stderr, "obs_reset_video failed (%d)\n", vr);
		obs_shutdown();
		return 2;
	}

	std::string bin, data;
	core_module(a.obs_dir, "image-source", bin, data);
	bool loaded = load_module(bin, data);
	for (const auto &f : a.filters) {
		if (f.id.rfind("glyphnotic", 0) != 0) {
			core_module(a.obs_dir, "obs-filters", bin, data);
			loaded = loaded && load_module(bin, data);
			break;
		}
	}
	loaded = loaded && load_module(a.plugin, a.plugin_data);
	if (!loaded) {
		obs_shutdown();
		return 2;
	}
	register_hdr_source();
	register_frames_source();

	int status = 0;
	std::vector<obs_source_t *> inputs;   // the stills
	std::vector<obs_source_t *> filters;  // every filter, owned here
	std::vector<obs_source_t *> filtered; // the source each filter is on
	obs_source_t *dump_filter = nullptr;  // first Glyphnotic filter of instance 0
	obs_scene_t *scene = nullptr;
	std::vector<obs_sceneitem_t *> items;

	if (a.on_scene || a.instances > 1)
		scene = obs_scene_create("scene");

	for (int i = 0; i < a.instances && status == 0; i++) {
		obs_data_t *s = obs_data_create();
		obs_data_set_string(s, "file", a.image.c_str());
		obs_data_set_double(s, "scale", a.hdr_scale);
		std::string files = a.image;
		for (const std::string &f : a.also)
			files += "\n" + f;
		obs_data_set_string(s, "files", files.c_str());
		obs_data_set_int(s, "period", a.period);
		obs_data_set_double(s, "noise", a.noise);
		obs_data_set_int(s, "seed", a.noise_seed + i);
		const std::string name = "still " + std::to_string(i);
		const char *kind = a.hdr_source      ? "harness_hdr_still"
				   : a.frames_source ? "harness_frames"
						     : "image_source";
		obs_source_t *input = obs_source_create(kind, name.c_str(), s, nullptr);
		obs_data_release(s);
		if (!input || obs_source_get_width(input) != w || obs_source_get_height(input) != h) {
			fprintf(stderr, "the still didn't load: %s\n", a.image.c_str());
			obs_source_release(input);
			status = 2;
			break;
		}
		inputs.push_back(input);
		if (scene) {
			obs_sceneitem_t *item = obs_scene_add(scene, input);
			struct vec2 pos = {{{a.pos_x, a.pos_y}}};
			obs_sceneitem_set_pos(item, &pos);
			items.push_back(item);
		}
		obs_source_t *target = (a.on_scene && a.instances == 1) ? obs_scene_get_source(scene) : input;
		for (size_t k = 0; k < a.filters.size(); k++) {
			obs_data_t *fs = obs_data_create_from_json(a.filters[k].settings.c_str());
			if (!fs) {
				fprintf(stderr, "bad --settings JSON for %s\n", a.filters[k].id.c_str());
				status = 2;
				break;
			}
			const std::string fname = a.filters[k].id + " " + std::to_string(i) + "." + std::to_string(k);
			obs_source_t *filter = obs_source_create(a.filters[k].id.c_str(), fname.c_str(), fs, nullptr);
			obs_data_release(fs);
			if (!filter || !obs_obj_get_data(filter)) {
				fprintf(stderr, "cannot create filter %s\n", a.filters[k].id.c_str());
				obs_source_release(filter);
				status = 2;
				break;
			}
			obs_source_filter_add(target, filter);
			filters.push_back(filter);
			filtered.push_back(target);
			if (i == 0 && !dump_filter && a.filters[k].id.rfind("glyphnotic", 0) == 0)
				dump_filter = filter;
		}
	}

	FrameGrab grab;
	// The frame is the still's area, or the whole canvas when --canvas is given.
	grab.width = a.canvas_w ? ovi.base_width : w;
	grab.height = a.canvas_h ? ovi.base_height : h;
	grab.bgra = ovi.output_format == VIDEO_FORMAT_BGRA;
	if (!a.frame.empty() && !grab.bgra) {
		fprintf(stderr, "--frame needs --format bgra; use --main-texture\n");
		status = 2;
	}

	obs_source_t *top = scene ? obs_scene_get_source(scene) : (inputs.empty() ? nullptr : inputs[0]);
	if (status == 0) {
		obs_set_output_source(0, top);
		obs_add_raw_video_callback(nullptr, raw_video, &grab);

		if (!wait_for([&] { return grab.count >= 5; }, 30000)) {
			fprintf(stderr, "no video frames\n");
			status = 2;
		}

		// Change settings as the properties window does: open the properties
		// (OBS runs every modified callback), set values, run each changed
		// property's modified callback, then update the filter.
		const bool ui = !a.ui_set.empty() || !a.press.empty();
		if (status == 0 && ui && dump_filter) {
			obs_properties_t *props = obs_source_properties(dump_filter);
			obs_data_t *settings = obs_source_get_settings(dump_filter);
			if (!a.ui_set.empty()) {
				obs_data_t *patch = obs_data_create_from_json(a.ui_set.c_str());
				if (!patch) {
					fprintf(stderr, "bad --ui-set JSON\n");
					status = 2;
				}
				// The window sets one value at a time; the order within the
				// patch doesn't matter for these tests, so apply it at once.
				obs_data_apply(settings, patch);
				for (obs_data_item_t *item = patch ? obs_data_first(patch) : nullptr; item;
				     obs_data_item_next(&item)) {
					const char *name = obs_data_item_get_name(item);
					obs_property_t *p = obs_properties_get(props, name);
					if (!p) {
						fprintf(stderr, "--ui-set: no property %s\n", name);
					} else if (obs_property_modified(p, settings)) {
						// The window would refresh: rebuild the properties.
						obs_properties_destroy(props);
						props = obs_source_properties(dump_filter);
					}
				}
				obs_data_release(patch);
				if (!a.ui_no_update)
					obs_source_update(dump_filter, nullptr);
			}
			for (const std::string &name : a.press) {
				obs_property_t *p = obs_properties_get(props, name.c_str());
				if (!p || obs_property_get_type(p) != OBS_PROPERTY_BUTTON) {
					fprintf(stderr, "--press: no button %s\n", name.c_str());
					status = 2;
				} else {
					obs_property_button_clicked(p, dump_filter);
				}
			}
			obs_data_release(settings);
			obs_properties_destroy(props);
			const int after_ui = grab.count;
			if (status == 0 && !wait_for([&] { return grab.count >= after_ui + a.wait_frames; }, 30000)) {
				fprintf(stderr, "no frames after --ui-set/--press\n");
				status = 2;
			}
		}

		if (status == 0 && !a.dump.empty() && !press_dump(dump_filter, a.dump))
			status = 2;
		if (status == 0 && !a.record.empty() && !press_record(dump_filter, a))
			status = 2;
		if (status == 0 && !a.frame_pair.empty()) {
			{
				std::lock_guard<std::mutex> lock(grab.mutex);
				grab.pair.clear();
				grab.pair_left = 2;
			}
			if (!wait_for(
				    [&] {
					    std::lock_guard<std::mutex> lock(grab.mutex);
					    return grab.pair.size() >= 2;
				    },
				    30000)) {
				fprintf(stderr, "no frame pair\n");
				status = 2;
			} else {
				std::lock_guard<std::mutex> lock(grab.mutex);
				for (int k = 0; k < 2 && status == 0; k++)
					if (!write_pam(a.frame_pair + "-" + std::to_string(k) + ".pam",
						       grab.pair[size_t(k)], grab.width, grab.height))
						status = 2;
			}
		}

		// Report which of a filter's properties are visible after OBS applies
		// the settings (running the modified callbacks, as the properties window does).
		if (status == 0 && !a.check_visible.empty() && dump_filter) {
			obs_properties_t *props = obs_source_properties(dump_filter);
			obs_data_t *settings = obs_source_get_settings(dump_filter);
			obs_properties_apply_settings(props, settings);
			obs_property_t *p = obs_properties_get(props, a.check_visible.c_str());
			fprintf(stderr, "visible %s: %s\n", a.check_visible.c_str(),
				!p                        ? "no such property"
				: obs_property_visible(p) ? "yes"
							  : "no");
			obs_data_release(settings);
			obs_properties_destroy(props);
		}

		if (status == 0 && dump_filter && !a.settings_out.empty()) {
			obs_data_t *settings = obs_source_get_settings(dump_filter);
			if (!write_file(a.settings_out, obs_data_get_json_pretty_with_defaults(settings)))
				status = 2;
			obs_data_release(settings);
		}
		if (status == 0 && dump_filter && !a.properties_out.empty()) {
			obs_properties_t *props = obs_source_properties(dump_filter);
			std::string json;
			properties_json(props, json);
			obs_properties_destroy(props);
			if (!write_file(a.properties_out, json))
				status = 2;
		}

		// The picture is still, so any later frame shows the same grid.
		const int after = grab.count;
		if (status == 0 && !wait_for([&] { return grab.count >= after + 3; }, 30000)) {
			fprintf(stderr, "no frames after the dump\n");
			status = 2;
		}
		if (status == 0 && !a.frame.empty()) {
			std::lock_guard<std::mutex> lock(grab.mutex);
			FILE *f = os_fopen(a.frame.c_str(), "wb");
			if (!f) {
				fprintf(stderr, "cannot write %s\n", a.frame.c_str());
				status = 2;
			} else {
				fprintf(f, "P7\nWIDTH %u\nHEIGHT %u\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n",
					grab.width, grab.height);
				fwrite(grab.rgba.data(), 1, grab.rgba.size(), f);
				fclose(f);
			}
		}
		// Switch the still (the first image source) and dump again.
		if (status == 0 && !a.then_image.empty() && !inputs.empty()) {
			obs_data_t *s = obs_data_create();
			obs_data_set_string(s, "file", a.then_image.c_str());
			obs_source_update(inputs[0], s);
			obs_data_release(s);
			const int after_switch = grab.count;
			if (!wait_for([&] { return grab.count >= after_switch + a.then_frames; }, 60000)) {
				fprintf(stderr, "no frames after --then-image\n");
				status = 2;
			}
			if (status == 0 && !a.dump2.empty() && !press_dump(dump_filter, a.dump2))
				status = 2;
		}
		if (status == 0 && !a.main_texture.empty() && !save_main_texture(a.main_texture)) {
			fprintf(stderr, "cannot save the main texture\n");
			status = 2;
		}

		// A timed run: lagged frames are frames OBS's video thread couldn't
		// render in time.
		if (status == 0 && a.frames > 0) {
			const uint32_t total0 = obs_get_total_frames(), lagged0 = obs_get_lagged_frames();
			const auto t0 = std::chrono::steady_clock::now();
			const int timeout = int(1000.0 * a.frames / a.fps * 3) + 10000;
			if (!wait_for([&] { return obs_get_total_frames() - total0 >= uint32_t(a.frames); }, timeout))
				fprintf(stderr, "timed out before %d frames\n", a.frames);
			const double secs =
				std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
			fprintf(stderr, "run: %u frames in %.2f s at %d fps, %u lagged\n",
				obs_get_total_frames() - total0, secs, a.fps, obs_get_lagged_frames() - lagged0);
		}
		if (a.profile) {
			for (size_t i = 0; i < inputs.size(); i++)
				print_profile("source", inputs[i]);
			if (scene)
				print_profile("scene", obs_scene_get_source(scene));
			for (obs_source_t *f : filters)
				print_profile("filter", f);
		}

		obs_remove_raw_video_callback(raw_video, &grab);
		obs_set_output_source(0, nullptr);
	}

	for (size_t i = 0; i < filters.size(); i++) {
		obs_source_filter_remove(filtered[i], filters[i]);
		obs_source_release(filters[i]);
	}
	for (obs_sceneitem_t *item : items)
		obs_sceneitem_remove(item);
	if (scene) {
		// OBS 33's main canvas keeps a reference to its scenes until they're removed.
		obs_source_remove(obs_scene_get_source(scene));
		obs_scene_release(scene);
	}
	for (obs_source_t *input : inputs)
		obs_source_release(input);
	// Sources are destroyed on OBS's destruction task queue; let it drain.
	obs_wait_for_destroy_queue();
	obs_shutdown();

	// OBS's source profiler doesn't free its entries, so --profile runs show
	// leaks of their own; check leaks without it.
	fprintf(stderr, "memory leaks: %ld%s\n", bnum_allocs(), a.profile ? " (includes OBS's source profiler)" : "");
	return status;
}

} // namespace

int main(int argc, char **argv)
{
	Args a;
	const bool parsed = parse_args(argc, argv, a);
	// Make paths absolute before the working directory changes.
	for (std::string *p : {&a.obs_dir, &a.plugin, &a.plugin_data, &a.image, &a.dump, &a.frame, &a.main_texture,
			       &a.settings_out, &a.properties_out, &a.then_image, &a.dump2, &a.record, &a.frame_pair}) {
		if (p->empty())
			continue;
		char *abs = os_get_abs_path_ptr(p->c_str());
		if (abs) {
			*p = abs;
			bfree(abs);
		}
	}
	for (std::string &p : a.also) {
		char *abs = os_get_abs_path_ptr(p.c_str());
		if (abs) {
			p = abs;
			bfree(abs);
		}
	}
	if (!parsed) {
		fputs(kUsage, stderr);
		return 2;
	}
	g_verbose = a.verbose;
	base_set_log_handler(log_handler, nullptr);
	return run(a);
}
