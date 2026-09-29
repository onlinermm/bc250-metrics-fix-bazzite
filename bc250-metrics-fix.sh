#!/usr/bin/bash
# bc250-metrics-fix -- BC-250 SMU metrics fix for Bazzite
#
# Run without arguments for the live dashboard (tui.py, Python curses), or
# with --menu for a simple menu. For scripts:
#
#   sudo ./bc250-metrics-fix.sh --install     build for every installed kernel, install, load
#   sudo ./bc250-metrics-fix.sh --uninstall   unload and remove everything (no reboot needed)
#   sudo ./bc250-metrics-fix.sh --set KEY=VAL sclk=extended|stock  hwmon=on|off
#   ./bc250-metrics-fix.sh --status           what is installed and loaded, all readings
#   ./bc250-metrics-fix.sh --selftest         consistency checks on the live readings
#   ./bc250-metrics-fix.sh --monitor          live readings, refreshed every second
#   ./bc250-metrics-fix.sh --build [DIR]      build for the running kernel only (no root)
#
# Nothing in /usr is touched and amdgpu is not replaced: the module and its
# load rules live under /etc, so removing them is a complete rollback.
set -euo pipefail
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin

VERSION=1.0
NAME=bc250-metrics-fix
MOD=bc250_metrics_fix
HWMON_NAME=bc250

SELF=$(realpath "$0")
HERE=$(dirname "$SELF")
ETC=/etc/$NAME
MODPROBE_CONF=/etc/modprobe.d/$NAME.conf
OPTIONS_CONF=/etc/modprobe.d/$NAME-options.conf
UDEV_RULE=/etc/udev/rules.d/90-$NAME.rules
CACHE=/var/cache/$NAME
FCONTEXT="$ETC(/.*)?"
MARKER="# managed by $NAME"
SRC_URL=https://github.com/OpenGamingCollective/linux
PCI_ID=1002:13fe

# ===========================================================================
# Output

if [ -t 1 ]; then
	C_OK=$'\033[32m' C_WARN=$'\033[33m' C_BAD=$'\033[31m' C_DIM=$'\033[2m'
	C_BOLD=$'\033[1m' C_ACC=$'\033[38;5;103m' C_OFF=$'\033[0m'
else
	C_OK= C_WARN= C_BAD= C_DIM= C_BOLD= C_ACC= C_OFF=
fi

say()  { printf '%s==>%s %s%s%s\n' "$C_ACC" "$C_OFF" "$C_BOLD" "$*" "$C_OFF"; }
ok()   { printf '  %s✔%s %s\n' "$C_OK" "$C_OFF" "$*"; }
warn() { printf '  %s!%s %s\n' "$C_WARN" "$C_OFF" "$*"; }
bad()  { printf '  %s✘%s %s\n' "$C_BAD" "$C_OFF" "$*"; }
die()  { printf '%serror:%s %s\n' "$C_BAD" "$C_OFF" "$*" >&2; exit 1; }

# ===========================================================================
# Kernels and sources

# "krel<TAB>kernel-devel dir": booted system first, then the other ostree
# deployments (a pending update, the rollback entry).
list_kernels() {
	local d
	for d in /usr/src/kernels/* /ostree/deploy/*/deploy/*/usr/src/kernels/*; do
		[ -f "$d/Makefile" ] || continue
		printf '%s\t%s\n' "${d##*/}" "$d"
	done 2>/dev/null | awk -F'\t' '!seen[$1]++'
}

# 7.2.4-ogc3.1.fc44.x86_64 -> v7.2.4-ogc3 (plus v7.2-ogcN for X.Y.0)
ogc_tags() {
	[[ $1 =~ ^([0-9]+\.[0-9]+)(\.[0-9]+)?(-rc[0-9]+)?-(ogc[0-9]+)[.-] ]] || return 1
	local mm=${BASH_REMATCH[1]} p=${BASH_REMATCH[2]} rc=${BASH_REMATCH[3]} ogc=${BASH_REMATCH[4]}
	echo "v$mm$p$rc-$ogc"
	if [ "$p" = .0 ]; then echo "v$mm$rc-$ogc"; fi
	return 0
}

# amdgpu's headers at the kernel's exact tag: a shallow, blobless, sparse
# fetch of drivers/gpu/drm/amd/**/*.h without the register headers the
# module does not use (~17 MB). Prints the amd directory.
fetch_headers() {
	local krel=$1 cache=$2 tag dir
	for tag in $(ogc_tags "$krel"); do
		dir=$cache/$tag
		if [ ! -f "$dir/.complete" ]; then
			rm -rf "$dir"
			mkdir -p "$dir"
			if ! {
				git -C "$dir" init -q &&
				git -C "$dir" remote add origin "$SRC_URL" &&
				git -C "$dir" sparse-checkout set --no-cone \
					'/drivers/gpu/drm/amd/**/*.h' \
					'!/drivers/gpu/drm/amd/include/asic_reg/**' \
					'/drivers/gpu/drm/amd/include/asic_reg/gc/gc_10_1_0_*.h' &&
				git -C "$dir" fetch -q --depth 1 --filter=blob:none origin tag "$tag" &&
				git -C "$dir" -c advice.detachedHead=false checkout -q FETCH_HEAD
			} >/dev/null 2>&1; then
				rm -rf "$dir"
				continue
			fi
			touch "$dir/.complete"
		fi
		echo "$dir/drivers/gpu/drm/amd"
		return 0
	done
	return 1
}

# build_one KREL KDIR OUT.ko CACHE -- needs only kernel-devel and git.
build_one() {
	local krel=$1 kdir=$2 out=$3 cache=$4 amd work vm
	amd=$(fetch_headers "$krel" "$cache") || {
		warn "$krel: no OpenGamingCollective/linux source tag ($(ogc_tags "$krel" 2>/dev/null | xargs))"
		return 1
	}
	work=$(mktemp -d)
	cp "$HERE/module/$MOD.c" "$HERE/module/Kbuild" "$work/"
	if ! make -C "$kdir" M="$work" AMD="$amd" -j"$(nproc)" modules >"$work/build.log" 2>&1; then
		tail -n 20 "$work/build.log" >&2
		warn "$krel: build failed"
		rm -rf "$work"
		return 1
	fi
	vm=$(modinfo -F vermagic "$work/$MOD.ko" | cut -d' ' -f1)
	if [ "$vm" != "$krel" ]; then
		warn "$krel: built module has vermagic $vm"
		rm -rf "$work"
		return 1
	fi
	mkdir -p "$(dirname "$out")"
	strip --strip-debug -o "$out.new" "$work/$MOD.ko"
	chmod 644 "$out.new"
	mv -f "$out.new" "$out"
	rm -rf "$work"
}

write_file() {	# write_file CONTENT DEST
	printf '%s\n' "$1" >"$2.new"
	chmod 644 "$2.new"
	mv -f "$2.new" "$2"
}

# ===========================================================================
# Earlier installs of this project under other names

REBOOT_NEEDED=0
remove_previous() {
	local found=0 karg=rd.driver.blacklist=amdgpu

	# The add-on under its previous name: unload it first, so its hooks are
	# gone before ours look for the stock callbacks.
	if [ -d /sys/module/bc250_telemetry ]; then
		rmmod bc250_telemetry 2>/dev/null || true
		found=1
	fi
	if [ -e /etc/bc250-telemetry ] || [ -e /etc/udev/rules.d/90-bc250-telemetry.rules ]; then
		found=1
		rm -rf /etc/bc250-telemetry /var/cache/bc250-telemetry
		rm -f /etc/udev/rules.d/90-bc250-telemetry.rules /etc/modprobe.d/bc250-telemetry.conf
		semanage fcontext -d '/etc/bc250-telemetry(/.*)?' 2>/dev/null || true
	fi

	# The first version, which replaced amdgpu.ko.
	if [ -e /usr/local/sbin/bc250-amdgpu-load ] || [ -e /etc/systemd/system/bc250-amdgpu-load.service ] ||
	   [ -e /etc/systemd/system/bc250-telemetry-build.timer ]; then
		found=1
		systemctl disable --now bc250-telemetry-build.timer 2>/dev/null || true
		systemctl disable bc250-amdgpu-load.service 2>/dev/null || true
		rm -f /etc/systemd/system/bc250-telemetry-build.service \
		      /etc/systemd/system/bc250-telemetry-build.timer \
		      /etc/systemd/system/bc250-amdgpu-load.service \
		      /usr/local/sbin/bc250-telemetry-build /usr/local/sbin/bc250-amdgpu-load \
		      /usr/local/bin/bc250-telemetry
		rm -rf /usr/local/lib/bc250-telemetry /var/lib/bc250-telemetry
		semanage fcontext -d '/var/lib/bc250-telemetry/modules(/.*)?' 2>/dev/null || true
		systemctl daemon-reload
	fi
	if rpm-ostree kargs 2>/dev/null | tr ' ' '\n' | grep -qx "$karg"; then
		found=1
		warn "removing kernel argument $karg (it keeps amdgpu from loading)"
		rpm-ostree kargs --delete-if-present="$karg" >/dev/null
		REBOOT_NEEDED=1
	fi
	if [ $found = 1 ]; then ok "removed an earlier install of this project"; fi
	return 0
}

# ===========================================================================
# Helpers for state

bc250_hwmon() {
	local h
	for h in /sys/class/hwmon/hwmon*; do
		if [ "$(cat "$h/name" 2>/dev/null)" = "$HWMON_NAME" ]; then echo "$h"; return 0; fi
	done
	return 1
}

# Older builds kept these files on the hwmon device only.
telemetry_dir() {
	if [ -f /sys/module/$MOD/telemetry/summary ]; then echo /sys/module/$MOD/telemetry; return 0; fi
	bc250_hwmon
}

hwmon_where() {
	local h
	h=$(cat /sys/module/$MOD/telemetry/hwmon 2>/dev/null || true)
	if [ -z "$h" ]; then h=$(bc250_hwmon || true); h=${h##*/}; fi
	if [ -n "$h" ] && [ "$h" != none ]; then echo "$h"; else echo off; fi
}

amdgpu_dev() {
	local d
	for d in /sys/class/drm/card[0-9]*; do
		if [ "$(cat "$d/device/device" 2>/dev/null)" = 0x13fe ]; then echo "$d/device"; return 0; fi
	done
	return 1
}

amdgpu_hwmon() {
	local d h
	d=$(amdgpu_dev) || return 1
	for h in "$d"/hwmon/hwmon*; do
		if [ "$(cat "$h/name" 2>/dev/null)" = amdgpu ]; then echo "$h"; return 0; fi
	done
	return 1
}

module_state() {
	local h
	if [ ! -d /sys/module/$MOD ]; then echo "not loaded"; return; fi
	h=$(telemetry_dir || true)
	echo "loaded v$(cat /sys/module/$MOD/version 2>/dev/null || echo ?), $(cat "$h/state" 2>/dev/null || echo ?), $(cat "$h/layout" 2>/dev/null || echo ?) layout"
}

built_kernels() {
	local k list=
	for k in "$ETC"/*/$MOD.ko; do
		if [ -f "$k" ]; then list+="$(basename "$(dirname "$k")") "; fi
	done
	echo "${list:-none}"
}

# Current settings: sclk=extended|stock, hwmon=on|off
setting() {
	local key=$1 line
	line=$(grep -E "^options $MOD " "$OPTIONS_CONF" 2>/dev/null || true)
	case $key in
	sclk)   if [[ $line == *sclk_max=2000* ]]; then echo stock; else echo extended; fi ;;
	hwmon)  if [[ $line == *hwmon=1* ]]; then echo on; else echo off; fi ;;
	esac
}

write_options() {	# write_options SCLK HWMON
	local opts=
	if [ "$1" = stock ]; then opts+=" sclk_min=1000 sclk_max=2000"; fi
	if [ "$2" = on ]; then opts+=" hwmon=1"; fi
	if [ -z "$opts" ]; then
		rm -f "$OPTIONS_CONF"
	else
		write_file "$MARKER
# Settings for $MOD (edit with: $NAME.sh, Settings)
options $MOD$opts" "$OPTIONS_CONF"
	fi
}

reload_module() {
	local args=() prev
	if [ ! -d /sys/module/amdgpu ]; then
		warn "amdgpu is not loaded; $MOD loads with it on the next boot"
		return 0
	fi
	if [ -d /sys/module/$MOD ]; then
		# Keep the boot-time default clock; a governor may have moved it.
		prev=$(cat /sys/module/$MOD/parameters/sclk_default 2>/dev/null || true)
		if [[ $prev =~ ^[1-9][0-9]*$ ]]; then args+=("sclk_default=$prev"); fi
		rmmod $MOD
	fi
	if modprobe $MOD "${args[@]}"; then
		ok "$MOD loaded: $(module_state)"
	else
		bad "loading $MOD failed: journalctl -k -g $MOD"
		return 1
	fi
}

# ===========================================================================
# Commands

cmd_install() {
	local booted krel kdir built=0 failed=0 f c
	booted=$(uname -r)

	say "$NAME $VERSION: install"
	[ -n "$(lspci -n -d $PCI_ID 2>/dev/null)" ] || die "no BC-250 GPU ($PCI_ID) found"
	for c in git make gcc strip modinfo insmod semanage restorecon udevadm rpm-ostree; do
		command -v "$c" >/dev/null || die "missing required tool: $c"
	done
	[ -f "/usr/src/kernels/$booted/Makefile" ] || die "kernel-devel for $booted is not installed"
	for f in "$MODPROBE_CONF" "$OPTIONS_CONF" "$UDEV_RULE"; do
		if [ -e "$f" ] && ! grep -qF "$MARKER" "$f"; then
			die "$f exists and was not created by $NAME"
		fi
	done

	remove_previous
	# Settings of earlier versions this one no longer has (socclk_dump).
	if [ -f "$OPTIONS_CONF" ]; then write_options "$(setting sclk)" "$(setting hwmon)"; fi

	say "Building $MOD"
	mkdir -p "$ETC"
	while IFS=$'\t' read -r krel kdir; do
		if build_one "$krel" "$kdir" "$ETC/$krel/$MOD.ko" "$CACHE"; then
			ok "$krel"
			built=$((built + 1))
		else
			failed=$((failed + 1))
			if [ "$krel" = "$booted" ]; then
				die "could not build for the running kernel $booted; no load rules were installed"
			fi
		fi
	done < <(list_kernels)

	# The kernel only loads module files labelled modules_object_t.
	semanage fcontext -a -t modules_object_t "$FCONTEXT" 2>/dev/null ||
		semanage fcontext -m -t modules_object_t "$FCONTEXT"
	restorecon -R "$ETC"

	say "Installing load rules"
	# modprobe resolves the name through this install rule alone, so the
	# module never has to be in the read-only /usr/lib/modules.
	write_file "$MARKER
# Load the $NAME module built for the running kernel, if there is one.
install $MOD f=$ETC/\$(uname -r)/$MOD.ko; /usr/bin/modprobe amdgpu && if [ -e \"\$f\" ]; then /usr/bin/insmod \"\$f\" \$CMDLINE_OPTS; fi
remove $MOD /usr/bin/rmmod $MOD" "$MODPROBE_CONF"
	# The BC-250's DRM card appearing (boot, or an amdgpu rebind) loads it.
	write_file "$MARKER
ACTION==\"add\", SUBSYSTEM==\"drm\", KERNEL==\"card[0-9]*\", ATTRS{vendor}==\"0x1002\", ATTRS{device}==\"0x13fe\", RUN+=\"/usr/bin/modprobe $MOD\"" "$UDEV_RULE"
	udevadm control --reload
	ok "$MODPROBE_CONF"
	ok "$UDEV_RULE"

	if [ $REBOOT_NEEDED = 1 ] || [ ! -d /sys/module/amdgpu ]; then
		echo
		say "Installed. Reboot once: amdgpu then loads normally and $MOD with it."
		return 0
	fi

	say "Loading $MOD"
	reload_module
	echo
	say "Done: $built kernel(s) built$([ $failed = 0 ] || echo ", $failed failed"), active now, no reboot needed."
}

cmd_uninstall() {
	say "$NAME: uninstall"
	if [ -d /sys/module/$MOD ]; then
		if rmmod $MOD; then ok "$MOD unloaded, amdgpu's own callbacks restored"
		else bad "rmmod failed; files are removed anyway, a reboot finishes it"; fi
	fi
	rm -f "$UDEV_RULE" "$MODPROBE_CONF" "$OPTIONS_CONF"
	rm -rf "$ETC" "$CACHE"
	semanage fcontext -d "$FCONTEXT" 2>/dev/null || true
	udevadm control --reload 2>/dev/null || true
	ok "files removed"
	remove_previous
	ok "amdgpu was never modified; nothing else to restore"
	if [ $REBOOT_NEEDED = 1 ]; then say "Reboot once to drop the kernel argument left by an earlier install."; fi
}

cmd_set() {	# cmd_set key=value...
	local sclk hwmon kv
	sclk=$(setting sclk)
	hwmon=$(setting hwmon)
	for kv in "$@"; do
		case $kv in
		sclk=extended|sclk=stock) sclk=${kv#*=} ;;
		hwmon=on|hwmon=off) hwmon=${kv#*=} ;;
		*) die "unknown setting '$kv' (sclk=extended|stock, hwmon=on|off)" ;;
		esac
	done
	[ -f "$MODPROBE_CONF" ] || die "$NAME is not installed"
	write_options "$sclk" "$hwmon"
	ok "sclk range: $sclk, bc250 hwmon device: $hwmon"
	reload_module
}

cmd_build() {
	local out krel
	out=$(realpath -m "${1:-$HERE/out}")
	krel=$(uname -r)
	[ -f "/usr/src/kernels/$krel/Makefile" ] || die "kernel-devel for $krel is not installed"
	say "Building $MOD for $krel"
	build_one "$krel" "/usr/src/kernels/$krel" "$out/$MOD-$krel.ko" \
		"${XDG_CACHE_HOME:-$HOME/.cache}/$NAME" || die "build failed"
	ok "$out/$MOD-$krel.ko"
}

cmd_status() {
	local h
	h=$(telemetry_dir || true)
	printf '%-12s %s\n' "kernel" "$(uname -r)"
	printf '%-12s %s\n' "amdgpu" "$([ -d /sys/module/amdgpu ] && echo loaded || echo 'NOT loaded')"
	printf '%-12s %s\n' "module" "$(module_state)"
	printf '%-12s %s\n' "built for" "$(built_kernels)"
	printf '%-12s %s\n' "load rules" "$([ -f "$MODPROBE_CONF" ] && [ -f "$UDEV_RULE" ] && echo installed || echo 'not installed')"
	if [ -d /sys/module/$MOD ]; then
		printf '%-12s %s\n' "sclk range" "$(setting sclk), $(cat /sys/module/$MOD/parameters/sclk_min)-$(cat /sys/module/$MOD/parameters/sclk_max) MHz"
	else
		printf '%-12s %s\n' "sclk range" "$(setting sclk)"
	fi
	if [ -d /sys/module/$MOD ]; then
		printf '%-12s %s\n' "hwmon" "$(setting hwmon), $(hwmon_where)"
	else
		printf '%-12s %s\n' "hwmon" "$(setting hwmon)"
	fi
	if [ ! -f "$ETC/$(uname -r)/$MOD.ko" ] && [ -f "$MODPROBE_CONF" ]; then
		warn "nothing built for the running kernel: run Install / update"
	fi
	if [ -n "$h" ]; then
		echo
		cat "$h/summary"
		echo "gpu busy: $(cat "$h/gpu_busy_percent")%"
		echo
		echo "${C_DIM}source: $h${C_OFF}"
		if [ "$(hwmon_where)" != off ]; then echo "${C_DIM}sensors: sensors $HWMON_NAME-*${C_OFF}"; fi
	fi
}

cmd_monitor() {
	local h key
	h=$(telemetry_dir) || die "$MOD is not loaded"
	trap 'tput cnorm 2>/dev/null; echo' EXIT
	tput civis 2>/dev/null || true
	while :; do
		printf '\033[H\033[2J'
		printf '%s%s monitor%s   %s   press q to quit\n\n' "$C_BOLD" "$NAME" "$C_OFF" "$(date +%T)"
		cat "$h/summary"
		echo "gpu busy: $(cat "$h/gpu_busy_percent")%"
		if read -rsn1 -t 1 key && [[ $key == [qQ] ]]; then break; fi
	done
	tput cnorm 2>/dev/null || true
	trap - EXIT
}

cmd_selftest() {
	local h fails=0 warns=0 tfail= layout cores want rows line mv a w exp ppt sock v d dev ah temps label
	h=$(telemetry_dir) || { bad "no telemetry: $MOD is not loaded"; return 1; }
	dev=$(amdgpu_dev || true)
	ah=$(amdgpu_hwmon || true)
	say "$NAME self-test ($(uname -r), $(cat "$h/state"))"

	# The layout comes from the table: patched SMU firmware keeps the
	# eight-core table with the extra cores locked.
	layout=$(cat "$h/layout")
	cores=$(grep -c '^core [0-9]:' "$h/summary" || true)
	want=$(lscpu -p=core 2>/dev/null | grep -v '^#' | sort -u | wc -l)
	if [ "$want" -ge 8 ]; then want=8; else want=6; fi
	if [ "$layout" = "$want-core" ]; then ok "layout $layout matches $want physical cores"
	elif [ "$layout" = 6-core ]; then
		bad "6-core table with 8 cores: the SMU firmware lacks the 8-core metrics patch; enable it in the BIOS"
		fails=$((fails + 1))
	else warn "8-core table with 6 cores (patched SMU firmware, cores locked)"; warns=$((warns + 1)); fi
	rows=${layout%-core}
	if [ "$want" -lt "$rows" ]; then rows=$want; fi
	if [ "$cores" = "$rows" ]; then ok "$cores core rows"
	else bad "$cores core rows, expected $rows"; fails=$((fails + 1)); fi

	for d in cpu gpu; do
		line=$(awk -v d="$d" '$1=="pd" && $2==d":"' "$h/summary")
		mv=$(awk '{sub(/mV/,"",$3); print $3}' <<<"$line")
		a=$(awk '{sub(/A/,"",$4); print $4}' <<<"$line")
		w=$(awk '{sub(/W/,"",$5); print $5}' <<<"$line")
		exp=$(awk -v v="$mv" -v a="$a" 'BEGIN{printf "%.2f", v/1000*a}')
		if awk -v e="$exp" -v w="$w" 'BEGIN{d=e-w; if(d<0)d=-d; exit !(d/(w<1?1:w)<=0.15)}'; then
			ok "pd $d: $mv mV x $a A = $exp W, reported $w W"
		else
			warn "pd $d: $mv mV x $a A = $exp W, reported $w W (more than 15% apart)"; warns=$((warns + 1))
		fi
		if ! awk -v x="$mv" 'BEGIN{exit !(x>=200 && x<=2000)}'; then
			bad "pd $d voltage $mv mV outside 200..2000 mV"; fails=$((fails + 1))
		fi
	done

	v=$(awk '$1=="gfx:" {sub(/MHz/,"",$2); print $2}' "$h/summary")
	if [ "${v:-0}" -ge 200 ] && [ "$v" -le 3000 ]; then ok "gfx clock $v MHz"
	else bad "gfx clock ${v:-?} MHz is out of range"; fails=$((fails + 1)); fi
	v=$(awk '$1=="core" {w=$4; sub(/W/,"",w); if (w+0 > 20) print $2 w}' "$h/summary")
	if [ -z "$v" ]; then ok "core power within 0..20 W"
	else bad "core power out of range: $(tr '\n' ' ' <<<"$v")"; fails=$((fails + 1)); fi

	sock=$(awk '$1=="pd" && $2=="socket:" {sub(/W/,"",$3); print $3}' "$h/summary")
	if [ -n "$ah" ]; then
		ppt=$(awk '{printf "%.2f", $1/1e6}' "$ah/power1_input" 2>/dev/null || true)
		if [ -n "$ppt" ] && awk -v a="$ppt" -v b="$sock" 'BEGIN{d=a-b; if(d<0)d=-d; exit !(d/(b<1?1:b)<=0.25)}'; then
			ok "amdgpu PPT $ppt W ~ SMU socket $sock W"
		else
			warn "amdgpu PPT ${ppt:-?} W vs SMU socket $sock W (samples are not simultaneous)"; warns=$((warns + 1))
		fi
	fi
	if [ -n "$dev" ]; then
		v=$(cat "$dev/gpu_busy_percent" 2>/dev/null || echo unsupported)
		if [[ $v =~ ^[0-9]+$ ]]; then ok "gpu_busy_percent $v%"
		else warn "gpu_busy_percent: $v"; warns=$((warns + 1)); fi
	fi

	# Each "NN.NNC" in the summary, labelled by its row.
	temps=$(awk '{
		for (i = 1; i <= NF; i++) if ($i ~ /^[0-9]+\.[0-9]+C$/) {
			l = ($1 == "core" || $1 == "L3") ? $1 " " $2 : $1; sub(/:$/, "", l)
			t = $i; sub(/C$/, "", t); print l "\t" int(t)
		}
	}' "$h/summary")
	while IFS=$'\t' read -r label v; do
		if [ "$v" -lt 10 ] || [ "$v" -gt 120 ]; then
			bad "$label at $v C is out of range"; fails=$((fails + 1)); tfail=1
		fi
	done <<<"$temps"
	if [ -z "$tfail" ]; then ok "all $(grep -c . <<<"$temps") temperatures within 10..120 C"; fi

	echo
	if [ $fails = 0 ]; then ok "self-test passed ($warns warning(s))"
	else bad "$fails failure(s), $warns warning(s)"; fi
	[ $fails = 0 ]
}

# ===========================================================================
# Interactive menu

have_gum() { command -v gum >/dev/null && [ -t 0 ] && [ -t 1 ]; }

as_root() {	# run this script with root for one action
	if [ $EUID -eq 0 ]; then "$SELF" "$@"; else sudo "$SELF" "$@"; fi
}

pause() {
	echo
	if have_gum; then
		gum style --faint "Press any key to return to the menu"
	else
		echo "Press any key to return to the menu"
	fi
	read -rsn1 _ || true
}

header() {
	local status
	status="kernel  $(uname -r)
amdgpu  $([ -d /sys/module/amdgpu ] && echo loaded || echo 'NOT loaded')
module  $(module_state)
built   $(built_kernels)"
	printf '\033[H\033[2J'
	if have_gum; then
		gum style --border rounded --border-foreground 60 --padding "0 2" --margin "1 1" \
			"$(gum style --bold --foreground 103 "BC-250 Bazzite Metrics Fix $VERSION")" \
			"$(gum style --faint 'SMU telemetry fix for amdgpu on Bazzite')" "" "$status"
	else
		echo
		echo "  BC-250 Bazzite Metrics Fix $VERSION"
		echo "  SMU telemetry fix for amdgpu on Bazzite"
		echo
		sed 's/^/  /' <<<"$status"
		echo
	fi
}

choose() {	# choose HEADER ITEM... -> prints the chosen item
	local head=$1 i n
	shift
	if have_gum; then
		gum choose --header "$head" --cursor "› " --height 12 "$@" || true
		return
	fi
	# The list and prompt go to stderr; only the choice goes to stdout.
	echo "$head" >&2
	n=1
	for i in "$@"; do echo "  $n) $i" >&2; n=$((n + 1)); done
	read -rp "> " n || return 0
	if [[ $n =~ ^[0-9]+$ ]] && [ "$n" -ge 1 ] && [ "$n" -le $# ]; then
		echo "${!n}"
	fi
}

confirm() {
	if have_gum; then
		gum confirm "$1"
	else
		local r
		read -rp "$1 [y/N] " r || return 1
		[[ $r == [yY]* ]]
	fi
}

menu_settings() {
	local sclk hwmon pick
	sclk=$(setting sclk)
	hwmon=$(setting hwmon)
	pick=$(choose "Settings" \
		"GFX clock range      [$sclk]" \
		"bc250 hwmon device   [$hwmon]" \
		"Back")
	case $pick in
	"GFX clock range"*)
		pick=$(choose "Range accepted through pp_od_clk_voltage" \
			"extended  350-2230 MHz (default)" \
			"stock     1000-2000 MHz")
		[ -n "$pick" ] || return 0
		as_root --set "sclk=${pick%% *}" || true
		pause ;;
	"bc250 hwmon"*)
		pick=$(choose "Register the bc250 hwmon device (every reading as a sensor)" \
			"off  telemetry only in /sys/module/$MOD/telemetry (default)" \
			"on   visible to sensors, KDE System Monitor, CoolerControl")
		[ -n "$pick" ] || return 0
		as_root --set "hwmon=${pick%% *}" || true
		pause ;;
	esac
}

menu() {
	local pick
	while :; do
		header
		pick=$(choose "What would you like to do?" \
			"Install / update" \
			"Status" \
			"Live monitor" \
			"Self-test" \
			"Settings" \
			"Uninstall" \
			"Build only (no install)" \
			"Quit")
		case $pick in
		"Install / update")
			as_root --install || true; pause ;;
		"Status")
			cmd_status || true; pause ;;
		"Live monitor")
			cmd_monitor || pause ;;
		"Self-test")
			cmd_selftest || true; pause ;;
		"Settings")
			menu_settings ;;
		"Uninstall")
			if confirm "Unload $MOD and remove all of its files?"; then
				as_root --uninstall || true
				pause
			fi ;;
		"Build only (no install)")
			cmd_build || true; pause ;;
		""|"Quit")
			printf '\033[H\033[2J'
			return 0 ;;
		esac
	done
}

# ===========================================================================

need_root() { if [ $EUID -ne 0 ]; then exec sudo "$SELF" "$@"; fi; }

case ${1:-} in
'')
	# The dashboard needs a terminal and curses.
	if [ -t 0 ] && [ -t 1 ] && python3 -c 'import curses' 2>/dev/null; then
		BC250_SCRIPT=$SELF exec python3 "$HERE/tui.py"
	fi
	menu ;;
--menu)       menu ;;
--install)    need_root "$@"; cmd_install ;;
--uninstall)  need_root "$@"; cmd_uninstall ;;
--set)        need_root "$@"; shift; cmd_set "$@" ;;
--status)     cmd_status ;;
--selftest)   cmd_selftest ;;
--monitor)    cmd_monitor ;;
--build)      cmd_build "${2:-}" ;;
--version)    echo "$NAME $VERSION" ;;
-h|--help)    sed -n '2,15p' "$SELF" | sed 's/^# \{0,1\}//' ;;
*)            die "unknown option $1 (see --help)" ;;
esac
