#!/usr/bin/env bash
# Glyphnotic's Linux install test (Phase 5). Runs inside a fresh ubuntu:26.04
# container with the repository mounted read-only at /src and an output
# folder at /out (see tools/README.md for the docker command):
#
#   1. installs OBS 33 from OBS's PPA;
#   2. takes the packages from /pkgs if it holds a glyphnotic-*.deb and a
#      glyphnotic-*-linux-x86_64.tar.xz (CI's, downloaded), or else builds
#      them with the scripts CI uses (.github/scripts/build-ubuntu and
#      package-ubuntu --package) into /out/packages;
#   3. follows docs/INSTALL.md's Linux steps exactly, as an ordinary user
#      with sudo: `sudo apt install ./<deb>`, start OBS (under xvfb-run, on a
#      collection with Glyphnotic Ink on an image source), `sudo apt remove
#      glyphnotic`, start OBS again; then the tarball: `mkdir -p` and
#      `tar -xJf ... -C ~/.local/share/obs-studio/plugins`, start OBS, delete
#      the glyphnotic folder, start OBS again;
#   4. checks each OBS log: with the plugin installed it loaded (with the
#      packages' version), the filter was created, no Glyphnotic errors, a
#      clean shutdown and 0 leaks; after uninstalling, OBS didn't load it and
#      no plugin files are left.
#
# Writes /out/install-*.log (OBS's logs) and /out/install-result.txt. Exits 0
# when every check passes.
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
RUN_SECONDS=${RUN_SECONDS:-30}

step() { echo; echo "=== $*"; }

step "Installing OBS 33"
apt-get update -q
apt-get install -y -q --no-install-recommends software-properties-common gpg-agent ca-certificates sudo \
	xvfb xauth procps zsh jq git file xz-utils
add-apt-repository -y ppa:obsproject/obs-studio
# As in CI (.github/scripts/utils.zsh/setup_ubuntu): on amd64v3 systems apt
# would read only the PPA's empty amd64v3 index.
for f in /etc/apt/sources.list.d/*obsproject*obs-studio*.sources; do
	grep -q '^Architectures:' "$f" || sed -i "/^Types:/a Architectures: $(dpkg --print-architecture)" "$f"
done
apt-get update -q
apt-get install -y -q --no-install-recommends obs-studio
obs_version=$(dpkg-query -W -f='${Version}' obs-studio)
echo "obs-studio ${obs_version}"
dpkg --compare-versions "${obs_version}" ge 33.0.0

pkgs=/out/packages
if compgen -G "/pkgs/glyphnotic-*-linux-x86_64.deb" >/dev/null && compgen -G "/pkgs/glyphnotic-*-linux-x86_64.tar.xz" >/dev/null; then
	step "Using the packages in /pkgs"
	pkgs=/pkgs
	source_of_packages="/pkgs (CI)"
else
	step "Building the packages as CI does"
	mkdir -p /work "$pkgs"
	tar -C /src --exclude='./.deps' --exclude='./.venv' --exclude='./build_*' --exclude='./release' -cf - . | tar -C /work -xf -
	cd /work
	CI=1 ./.github/scripts/build-ubuntu --target ubuntu-x86_64 --config RelWithDebInfo
	CI=1 ./.github/scripts/package-ubuntu --target ubuntu-x86_64 --config RelWithDebInfo --package
	cp release/glyphnotic-*-linux-x86_64.deb release/glyphnotic-*-linux-x86_64.tar.xz "$pkgs/"
	cd /
	source_of_packages="built here with build-ubuntu and package-ubuntu --package"
fi
deb=$(ls "$pkgs"/glyphnotic-*-linux-x86_64.deb | head -1)
tarball=$(ls "$pkgs"/glyphnotic-*-linux-x86_64.tar.xz | head -1)
version=$(dpkg-deb -f "$deb" Version)
ls -l "$deb" "$tarball"
dpkg-deb -I "$deb"
dpkg-deb -c "$deb"
tar -tJvf "$tarball"

step "Setting up the user and the test collection"
useradd -m -s /bin/bash tester || true
echo 'tester ALL=(ALL) NOPASSWD: ALL' >/etc/sudoers.d/tester
cp "$deb" "$tarball" /home/tester/
cfg=/home/tester/.config/obs-studio
mkdir -p "$cfg/basic/scenes" "$cfg/basic/profiles/Glyphnotic_Test" /home/tester/stills
cp /src/tests/stills/victor.png /home/tester/stills/
printf '[General]\nFirstRun=true\n' >"$cfg/global.ini"
printf '[General]\nFirstRun=true\n' >"$cfg/user.ini"
printf '[General]\nName=Glyphnotic Test\n\n[Video]\nBaseCX=1280\nBaseCY=720\nOutputCX=1280\nOutputCY=720\nFPSType=0\nFPSCommon=30\n' \
	>"$cfg/basic/profiles/Glyphnotic_Test/basic.ini"
cat >"$cfg/basic/scenes/Glyphnotic_Test.json" <<'EOF'
{"name": "Glyphnotic Test", "current_scene": "Scene", "current_program_scene": "Scene",
 "scene_order": [{"name": "Scene"}], "sources": [
 {"name": "Victor", "uuid": "10000000-0000-4000-8000-000000000001", "id": "image_source", "versioned_id": "image_source",
  "settings": {"file": "/home/tester/stills/victor.png"}, "enabled": true,
  "filters": [{"name": "Glyphnotic Ink", "uuid": "20000000-0000-4000-8000-000000000001", "id": "glyphnotic_ink",
               "versioned_id": "glyphnotic_ink", "settings": {"look": "black"}, "enabled": true}]},
 {"name": "Scene", "uuid": "30000000-0000-4000-8000-000000000001", "id": "scene", "versioned_id": "scene",
  "settings": {"id_counter": 1, "custom_size": false, "items": [
   {"name": "Victor", "source_uuid": "10000000-0000-4000-8000-000000000001", "id": 1, "visible": true, "locked": false,
    "rot": 0.0, "align": 5, "pos": {"x": 0.0, "y": 0.0}, "scale": {"x": 1.0, "y": 1.0}, "bounds_type": 0,
    "bounds": {"x": 0.0, "y": 0.0}}]}}]}
EOF
cp "$cfg/basic/scenes/Glyphnotic_Test.json" /home/tester/collection.json
cat >/home/tester/run-obs.sh <<EOF
#!/usr/bin/env bash
# The collection is rewritten before every run, so each run starts the same.
cp /home/tester/collection.json "$cfg/basic/scenes/Glyphnotic_Test.json"
obs --collection "Glyphnotic Test" --profile "Glyphnotic Test" --disable-shutdown-check --disable-updater &
pid=\$!
sleep ${RUN_SECONDS}
kill -INT \$pid
for i in \$(seq 60); do kill -0 \$pid 2>/dev/null || break; sleep 1; done
if kill -0 \$pid 2>/dev/null; then echo "OBS didn't exit after SIGINT"; kill -9 \$pid; fi
wait \$pid || true
EOF
chmod +x /home/tester/run-obs.sh
chown -R tester:tester /home/tester

as_tester() { su - tester -c "$1"; }
run_obs() { # name: runs OBS once as tester, copies its log to /out/install-<name>.log
	as_tester "xvfb-run -a -s '-screen 0 1280x720x24' /home/tester/run-obs.sh" || true
	cp "$(ls -t "$cfg"/logs/*.txt | head -1)" "/out/install-$1.log"
}

fail=0
result=/out/install-result.txt
: >"$result"
say() { echo "$*" | tee -a "$result"; }
check() { # description, file, grep -E pattern, expected count (exact) or "+" for at least one
	local n
	n=$(grep -cE -e "$3" "$2" || true)
	if [[ "$4" == "+" && "$n" -ge 1 ]] || [[ "$4" != "+" && "$n" -eq "$4" ]]; then
		say "pass: $1 ($n)"
	else
		say "FAIL: $1 ($n lines match '$3')"
		fail=1
	fi
}
check_installed() { # name
	local log=/out/install-$1.log
	check "$1: plugin loaded, version ${version}" "$log" "\[glyphnotic\] plugin loaded successfully \(version ${version}\)" 1
	check "$1: filter created" "$log" "- filter: 'Glyphnotic Ink' \(glyphnotic_ink\)" 1
	check "$1: no Glyphnotic errors or warnings" "$log" '\[glyphnotic\].*(error|failed|warning|cannot)' 0
	check "$1: no failed draws" "$log" 'device_draw .* failed|Could not find attribute' 0
	check "$1: clean shutdown" "$log" '\[glyphnotic\] plugin unloaded' 1
	check "$1: no leaks" "$log" 'Number of memory leaks: 0$' 1
}
check_removed() { # name, paths that must be gone...
	local name=$1 log=/out/install-$1.log
	shift
	check "$name: plugin not loaded" "$log" '\[glyphnotic\] plugin loaded' 0
	check "$name: clean shutdown" "$log" 'Number of memory leaks: 0$' 1
	# OBS keeps a filter whose plugin is missing, with its settings, in the saved collection.
	check "$name: the filter is still in the saved collection" "$cfg/basic/scenes/Glyphnotic_Test.json" \
		'"(id|versioned_id)": "glyphnotic_ink"' +
	check "$name: with its look" "$cfg/basic/scenes/Glyphnotic_Test.json" '"look": "black"' +
	for p in "$@"; do
		if [[ -e "$p" ]]; then
			say "FAIL: $name: $p is still there"
			fail=1
		else
			say "pass: $name: $p is gone"
		fi
	done
}

say "OBS ${obs_version} from OBS's PPA; Glyphnotic ${version}, packages ${source_of_packages}"
say "$(basename "$deb") $(stat -c %s "$deb") bytes, $(basename "$tarball") $(stat -c %s "$tarball") bytes"

step "The .deb: install, start OBS, remove, start OBS"
as_tester "cd ~ && sudo apt install -y ./$(basename "$deb")"
so=/usr/lib/x86_64-linux-gnu/obs-modules/plugins/glyphnotic.so
data=/usr/share/obs/obs-modules/plugins/glyphnotic
check "deb: installed glyphnotic.so" <(ls "$so" 2>&1) 'glyphnotic.so$' 1
check "deb: installed data" <(ls "$data" 2>&1) '^(effects|fonts|locale|manifest.json)$' 4
run_obs deb
check_installed deb
as_tester "sudo apt remove -y glyphnotic"
run_obs deb-removed
check_removed deb-removed "$so" "$data"

step "The tarball: extract, start OBS, delete the folder, start OBS"
as_tester "mkdir -p ~/.local/share/obs-studio/plugins"
as_tester "tar -xJf ~/$(basename "$tarball") -C ~/.local/share/obs-studio/plugins"
plug=/home/tester/.local/share/obs-studio/plugins/glyphnotic
check "tarball: glyphnotic.so and data in place" <(ls "$plug" "$plug/data" 2>&1) '^(glyphnotic.so|effects|fonts|locale|manifest.json)$' 5
check "tarball: glyphnotic.so stripped" <(file -L "$plug/glyphnotic.so") ', stripped$' 1
run_obs tarball
check_installed tarball
as_tester "rm -r ~/.local/share/obs-studio/plugins/glyphnotic"
run_obs tarball-removed
check_removed tarball-removed "$plug"

grep -q FAIL "$result" && fail=1
exit $fail
