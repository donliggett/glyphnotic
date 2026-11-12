-- Glyphnotic's Phase 5 tests in the OBS window (docs/SPEC.md, Build phases:
-- Phase 5). run_obs_tests.py adds this script to the test scene collection
-- and starts OBS with GLYPHNOTIC_TEST set to one of:
--
--   perf:<scene>:<seconds>   switch to <scene>, wait 10 s, record for
--                            <seconds>, stop; log the frames rendered and
--                            lagged and OBS's average frame render time
--   leak:<cycles>            on the source "P5 leak", add and remove each
--                            Glyphnotic filter <cycles> times, one at a time,
--                            each shown for about 6 frames
--   persist-set              on "P5 A", "P5 B" and "P5 C", add filters and
--                            change their settings as the properties window
--                            would (Capture palette included); log the
--                            settings and take a screenshot of each source
--   persist-check            after a restart: log the settings and take the
--                            screenshots again; then change A's look and log
--                            and screenshot all three once more
--   idle:<scene>:<seconds>   show <scene> for <seconds> and do nothing else
--
-- Without GLYPHNOTIC_TEST it does nothing. Every line it logs starts with
-- "[glyphnotic-tests.lua] P5 "; the last is "P5 done".
-- It's a test tool and is never shipped.

obs = obslua

local mode = os.getenv("GLYPHNOTIC_TEST") or ""
local steps = {}
local step_index = 0
local wait_until = 0
local clock = 0

function script_description()
	return "Glyphnotic's Phase 5 OBS tests (tools/obs-tests). Does nothing unless OBS is started with GLYPHNOTIC_TEST set."
end

local function log(fmt, ...)
	obs.script_log(obs.LOG_INFO, "P5 " .. string.format(fmt, ...))
end

local function wait(seconds)
	wait_until = clock + seconds
end

local function find_source(name)
	local s = obs.obs_get_source_by_name(name)
	if s == nil then
		log("ERROR: no source named '%s'", name)
	end
	return s
end

local function set_scene(name)
	local s = find_source(name)
	if s ~= nil then
		obs.obs_frontend_set_current_scene(s)
		obs.obs_source_release(s)
	end
end

local function with_filter(source_name, filter_name, fn)
	local src = find_source(source_name)
	if src == nil then
		return
	end
	local f = obs.obs_source_get_filter_by_name(src, filter_name)
	if f == nil then
		log("ERROR: no filter '%s' on '%s'", filter_name, source_name)
	else
		fn(f, src)
		obs.obs_source_release(f)
	end
	obs.obs_source_release(src)
end

local function update(source_name, filter_name, json)
	with_filter(source_name, filter_name, function(f)
		local d = obs.obs_data_create_from_json(json)
		obs.obs_source_update(f, d)
		obs.obs_data_release(d)
	end)
end

local function log_settings(source_name, filter_name)
	with_filter(source_name, filter_name, function(f)
		local d = obs.obs_source_get_settings(f)
		-- One line: OBS's log keeps a message to one line per call.
		local json = obs.obs_data_get_json(d):gsub("%s+", " ")
		log("settings %s / %s: %s", source_name, filter_name, json)
		obs.obs_data_release(d)
	end)
end

local function screenshot(source_name, tag)
	local src = find_source(source_name)
	if src ~= nil then
		log("screenshot %s %s", tag, source_name)
		obs.obs_frontend_take_source_screenshot(src)
		obs.obs_source_release(src)
	end
end

local function add_filter(source_name, id, filter_name, json)
	local src = find_source(source_name)
	if src == nil then
		return
	end
	local d = obs.obs_data_create_from_json(json)
	local f = obs.obs_source_create(id, filter_name, d, nil)
	obs.obs_data_release(d)
	obs.obs_source_filter_add(src, f)
	obs.obs_source_release(f)
	obs.obs_source_release(src)
end

local function press(source_name, filter_name, button)
	with_filter(source_name, filter_name, function(f)
		local props = obs.obs_source_properties(f)
		local p = obs.obs_properties_get(props, button)
		if p == nil then
			log("ERROR: no button '%s'", button)
		else
			obs.obs_property_button_clicked(p, f)
		end
		obs.obs_properties_destroy(props)
	end)
end

-- perf ----------------------------------------------------------------------

-- No frontend event callback here: the steps run in script_tick, on OBS's
-- graphics thread, and some frontend calls (switching scenes) wait for the UI
-- thread, which would wait for this script's lock to deliver the event:
-- OBS hangs. The steps poll instead.
local perf = {}

local function perf_steps(scene, seconds)
	return {
		function()
			set_scene(scene)
			log("perf scene '%s', %d s", scene, seconds)
			wait(10)
		end,
		function()
			perf.total0 = obs.obs_get_total_frames()
			perf.lagged0 = obs.obs_get_lagged_frames()
			perf.time = 0
			perf.samples = 0
			obs.obs_frontend_recording_start()
			wait(seconds)
		end,
		function()
			local frames = obs.obs_get_total_frames() - perf.total0
			local lagged = obs.obs_get_lagged_frames() - perf.lagged0
			log("perf result: %d frames, %d lagged, average frame render time %.3f ms (sampled), active fps %.2f",
				frames, lagged, perf.time / math.max(perf.samples, 1) / 1e6, obs.obs_get_active_fps())
			perf.stopping = true
			obs.obs_frontend_recording_stop()
			wait(1)
		end,
		function()
			if obs.obs_frontend_recording_active() then
				wait(1)
				return false -- run this step again
			end
			wait(2)
		end,
	}
end

-- leak ----------------------------------------------------------------------

local leak = {ids = {"glyphnotic_ink", "glyphnotic_mosaic", "glyphnotic_poster", "glyphnotic_console"}}

local function leak_steps(cycles)
	return {
		function()
			set_scene("Phase 5 leaks")
			leak.n = 0
			leak.total = cycles * #leak.ids
			log("leak test: %d filters, one at a time", leak.total)
			wait(3)
		end,
		function()
			local src = find_source("P5 leak")
			if src == nil then
				return
			end
			if leak.current ~= nil then
				obs.obs_source_filter_remove(src, leak.current)
				obs.obs_source_release(leak.current)
				leak.current = nil
			end
			if leak.n < leak.total then
				leak.n = leak.n + 1
				local id = leak.ids[(leak.n - 1) % #leak.ids + 1]
				local d = obs.obs_data_create()
				leak.current = obs.obs_source_create(id, "P5 leak " .. leak.n, d, nil)
				obs.obs_data_release(d)
				obs.obs_source_filter_add(src, leak.current)
				obs.obs_source_release(src)
				wait(0.1)
				return false -- again, until every filter has been added and removed
			end
			obs.obs_source_release(src)
			log("leak test: added and removed %d filters", leak.n)
			wait(3)
		end,
	}
end

-- persist -------------------------------------------------------------------

local ink_custom = [[{"charset": "custom", "custom_chars": " .:-=+*#%@", "ink_color": "custom",
	"ink_custom_color": 4282434815, "ink_background": "tint", "gamma": 1.2, "hold": 60, "soften": 1.0,
	"cell_scale": 2, "alignment": "top_left", "edges": "transparent"}]]

local function log_all(tag)
	log_settings("P5 A", "P5 Ink")
	log_settings("P5 B", "P5 Console")
	log_settings("P5 C", "P5 Poster")
	log("settings logged %s", tag)
end

local function shots(tag)
	return {
		function() screenshot("P5 A", tag); wait(1.5) end,
		function() screenshot("P5 B", tag); wait(1.5) end,
		function() screenshot("P5 C", tag); wait(1.5) end,
	}
end

local function append(list, more)
	for _, s in ipairs(more) do
		table.insert(list, s)
	end
	return list
end

local function persist_set_steps()
	local s = {
		function()
			set_scene("Phase 5 settings")
			add_filter("P5 A", "glyphnotic_ink", "P5 Ink", '{"look": "black"}')
			add_filter("P5 B", "glyphnotic_console", "P5 Console", '{"look": "console"}')
			add_filter("P5 C", "glyphnotic_poster", "P5 Poster", '{"look": "poster"}')
			wait(2)
		end,
		function()
			press("P5 B", "P5 Console", "capture_palette")
			update("P5 A", "P5 Ink", ink_custom)
			update("P5 B", "P5 Console", '{"hold": 40, "settle": 50, "charset": "ascii"}')
			update("P5 C", "P5 Poster", '{"poster_levels": 3, "cell_scale": 3}')
			wait(4)
		end,
		function() log_all("after set"); wait(1) end,
	}
	return append(s, shots("set"))
end

local function persist_check_steps()
	local s = {
		function() set_scene("Phase 5 settings"); wait(4) end,
		function() log_all("after restart"); wait(1) end,
	}
	append(s, shots("restart"))
	append(s, {
		function()
			update("P5 A", "P5 Ink", '{"look": "phosphor"}')
			wait(3)
		end,
		function() log_all("after changing A"); wait(1) end,
	})
	return append(s, shots("changed"))
end

-- driver --------------------------------------------------------------------

local function tick(seconds)
	clock = clock + seconds
	if perf.total0 ~= nil and not perf.stopping then
		perf.time = perf.time + obs.obs_get_average_frame_time_ns()
		perf.samples = perf.samples + 1
	end
	if step_index == 0 or clock < wait_until then
		return
	end
	local step = steps[step_index]
	if step == nil then
		log("done")
		step_index = 0
		return
	end
	local again = step() == false
	if not again then
		step_index = step_index + 1
	end
end

function script_tick(seconds)
	tick(seconds)
end

function script_load(settings)
	if mode == "" then
		return
	end
	local kind, a, b = mode:match("^([%w-]+):?([^:]*):?(.*)$")
	if kind == "perf" then
		steps = perf_steps(a, tonumber(b) or 60)
	elseif kind == "leak" then
		steps = leak_steps(tonumber(a) or 100)
	elseif kind == "persist-set" then
		steps = persist_set_steps()
	elseif kind == "persist-check" then
		steps = persist_check_steps()
	elseif kind == "idle" then
		-- OBS with the script loaded and nothing else: a baseline for the log's checks.
		steps = {function() set_scene(a ~= "" and a or "Phase 5 leaks"); wait(tonumber(b) or 10) end}
	else
		log("ERROR: unknown GLYPHNOTIC_TEST '%s'", mode)
		return
	end
	log("mode %s", mode)
	-- Start once OBS has finished loading the collection.
	step_index = 1
	wait(5)
end
