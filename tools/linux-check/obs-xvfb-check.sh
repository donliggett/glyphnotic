#!/usr/bin/env bash
# Glyphnotic's end-of-phase Linux check. Runs inside a fresh ubuntu:26.04
# container with the repository mounted read-only at /src and an output
# folder at /out (see tools/README.md for the docker command):
#
#   1. installs OBS 33 from OBS's PPA and the build tools;
#   2. builds the plugin as CI does (warnings as errors) and runs the unit tests;
#   3. installs it to the system plugin location (/usr, the .deb's layout);
#   4. runs OBS under xvfb-run, as an ordinary user, on a test collection with
#      one image source per Glyphnotic filter, plus Ink's two boiling looks
#      (Static and Fever, which run Boil's hashing and Settle), then stops OBS
#      with SIGINT so it shuts down cleanly and logs its leak count;
#   5. checks the log: the plugin loaded, all four filters were created, no
#      Glyphnotic errors, no failed draws, "Number of memory leaks: 0".
#
# Writes /out/obs.log, /out/screen.xwd (the X screen, OBS's window) and
# /out/result.txt. Exits 0 when every check passes.
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
RUN_SECONDS=${RUN_SECONDS:-40}

step() { echo; echo "=== $*"; }

step "Installing OBS 33 and the build tools"
apt-get update -q
apt-get install -y -q --no-install-recommends software-properties-common gpg-agent ca-certificates
add-apt-repository -y ppa:obsproject/obs-studio
# As in CI (.github/scripts/utils.zsh/setup_ubuntu): on amd64v3 systems apt
# would read only the PPA's empty amd64v3 index.
for f in /etc/apt/sources.list.d/*obsproject*obs-studio*.sources; do
	grep -q '^Architectures:' "$f" || sed -i "/^Types:/a Architectures: $(dpkg --print-architecture)" "$f"
done
apt-get update -q
apt-get install -y -q --no-install-recommends obs-studio build-essential cmake ninja-build pkg-config \
	libsimde-dev libgles2-mesa-dev xvfb xauth x11-apps procps
obs_version=$(dpkg-query -W -f='${Version}' obs-studio)
echo "obs-studio ${obs_version}"
dpkg --compare-versions "${obs_version}" ge 33.0.0

step "Building (warnings as errors) and running the unit tests"
mkdir -p /work
tar -C /src --exclude='./.deps' --exclude='./.venv' --exclude='./build_*' --exclude='./release' -cf - . | tar -C /work -xf -
cd /work
cmake --preset ubuntu-x86_64 -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_COMPILE_WARNING_AS_ERROR=ON
cmake --build --preset ubuntu-x86_64
cmake -S tests/unit -B build_tests -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build_tests
ctest --test-dir build_tests --output-on-failure

step "Installing to /usr"
cmake --install build_x86_64 --prefix /usr
ls -l /usr/lib/x86_64-linux-gnu/obs-modules/plugins/glyphnotic.so /usr/share/obs/obs-modules/plugins/glyphnotic

step "Writing the test collection and profile"
useradd -m tester || true
cfg=/home/tester/.config/obs-studio
mkdir -p "$cfg/basic/scenes" "$cfg/basic/profiles/Glyphnotic_Test" /home/tester/stills
cp /src/tests/stills/victor.png /src/tests/stills/bbb-alpha.png /home/tester/stills/
printf '[General]\nFirstRun=true\n' >"$cfg/global.ini"
printf '[General]\nFirstRun=true\n' >"$cfg/user.ini"
printf '[General]\nName=Glyphnotic Test\n\n[Video]\nBaseCX=1920\nBaseCY=1080\nOutputCX=1920\nOutputCY=1080\nFPSType=0\nFPSCommon=30\n' \
	>"$cfg/basic/profiles/Glyphnotic_Test/basic.ini"

# One image source per filter and two more for Ink's boiling looks, in a 3x2
# layout at half size.
source_json() { # name uuid file filter_id filter_name filter_uuid look
	cat <<EOF
{"name": "$1", "uuid": "$2", "id": "image_source", "versioned_id": "image_source",
 "settings": {"file": "/home/tester/stills/$3"}, "enabled": true,
 "filters": [{"name": "$5", "uuid": "$6", "id": "$4", "versioned_id": "$4", "settings": {"look": "$7"}, "enabled": true}]}
EOF
}
item_json() { # name uuid id x y
	cat <<EOF
{"name": "$1", "source_uuid": "$2", "id": $3, "visible": true, "locked": false, "rot": 0.0, "align": 5,
 "pos": {"x": $4, "y": $5}, "scale": {"x": 0.5, "y": 0.5}, "bounds_type": 0, "bounds": {"x": 0.0, "y": 0.0}}
EOF
}
{
	echo '{"name": "Glyphnotic Test", "current_scene": "Scene", "current_program_scene": "Scene",'
	echo ' "scene_order": [{"name": "Scene"}], "sources": ['
	source_json "Ink" 10000000-0000-4000-8000-000000000001 victor.png glyphnotic_ink "Glyphnotic Ink" 20000000-0000-4000-8000-000000000001 tint
	echo ','
	source_json "Mosaic" 10000000-0000-4000-8000-000000000002 bbb-alpha.png glyphnotic_mosaic "Glyphnotic Mosaic" 20000000-0000-4000-8000-000000000002 blocks
	echo ','
	source_json "Poster" 10000000-0000-4000-8000-000000000003 bbb-alpha.png glyphnotic_poster "Glyphnotic Poster" 20000000-0000-4000-8000-000000000003 poster
	echo ','
	source_json "Console" 10000000-0000-4000-8000-000000000004 victor.png glyphnotic_console "Glyphnotic Console" 20000000-0000-4000-8000-000000000004 console
	echo ','
	source_json "Ink Static" 10000000-0000-4000-8000-000000000005 bbb-alpha.png glyphnotic_ink "Glyphnotic Ink Static" 20000000-0000-4000-8000-000000000005 static
	echo ','
	source_json "Ink Fever" 10000000-0000-4000-8000-000000000006 victor.png glyphnotic_ink "Glyphnotic Ink Fever" 20000000-0000-4000-8000-000000000006 fever
	echo ', {"name": "Scene", "uuid": "30000000-0000-4000-8000-000000000001", "id": "scene", "versioned_id": "scene",'
	echo '   "settings": {"id_counter": 6, "custom_size": false, "items": ['
	item_json "Ink" 10000000-0000-4000-8000-000000000001 1 0.0 0.0
	echo ','
	item_json "Mosaic" 10000000-0000-4000-8000-000000000002 2 640.0 0.0
	echo ','
	item_json "Poster" 10000000-0000-4000-8000-000000000003 3 640.0 540.0
	echo ','
	item_json "Console" 10000000-0000-4000-8000-000000000004 4 0.0 540.0
	echo ','
	item_json "Ink Static" 10000000-0000-4000-8000-000000000005 5 1280.0 0.0
	echo ','
	item_json "Ink Fever" 10000000-0000-4000-8000-000000000006 6 1280.0 540.0
	echo ']}}]}'
} >"$cfg/basic/scenes/Glyphnotic_Test.json"
chown -R tester:tester /home/tester

step "Running OBS under xvfb-run for ${RUN_SECONDS} s"
cat >/home/tester/run-obs.sh <<EOF
#!/usr/bin/env bash
obs --collection "Glyphnotic Test" --profile "Glyphnotic Test" --disable-shutdown-check --disable-updater &
pid=\$!
sleep ${RUN_SECONDS}
xwd -root -silent >/tmp/screen.xwd || true
kill -INT \$pid
for i in \$(seq 60); do kill -0 \$pid 2>/dev/null || break; sleep 1; done
if kill -0 \$pid 2>/dev/null; then echo "OBS didn't exit after SIGINT"; kill -9 \$pid; fi
wait \$pid || true
EOF
chmod +x /home/tester/run-obs.sh
su - tester -c "xvfb-run -a -s '-screen 0 1920x1080x24' /home/tester/run-obs.sh" || true
log=$(ls -t "$cfg"/logs/*.txt | head -1)
cp "$log" /out/obs.log
cp /tmp/screen.xwd /out/screen.xwd 2>/dev/null || true

step "Checking the log"
fail=0
check() { # description, grep -E pattern, expected count (exact) or "+" for at least one
	local n
	n=$(grep -cE -e "$2" /out/obs.log || true)
	if [[ "$3" == "+" && "$n" -ge 1 ]] || [[ "$3" != "+" && "$n" -eq "$3" ]]; then
		echo "pass: $1 ($n)"
	else
		echo "FAIL: $1 ($n lines match '$2')"
		fail=1
	fi
}
{
	echo "$(grep -m1 -oE 'OBS [0-9.]+ \(linux\)' /out/obs.log || echo '?'), plugin from /usr"
	grep -m1 -E 'Loading up OpenGL on adapter' /out/obs.log || true
	check "plugin loaded" '\[glyphnotic\] plugin loaded successfully' 1
	for id in glyphnotic_ink glyphnotic_mosaic glyphnotic_poster glyphnotic_console; do
		check "filter $id created" "- filter: '.*' \($id\)" +
	done
	check "no Glyphnotic errors or warnings" '\[glyphnotic\].*(error|failed|warning|cannot)' 0
	check "no effect load errors" 'failed to load .*\.effect|effect file .* not found' 0
	check "no failed draws" 'device_draw .* failed|Could not find attribute' 0
	check "clean shutdown" '\[glyphnotic\] plugin unloaded' 1
	check "no leaks" 'Number of memory leaks: 0$' 1
} | tee /out/result.txt
grep -q FAIL /out/result.txt && fail=1
exit $fail
