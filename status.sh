#!/bin/sh
# pluto-bar status: music, CPU, RAM, volume, microphone, battery, clock.
# Fixed version with correct functionality and symbol placeholders.

# ============================================================================
# CONFIGURATION SECTION
# ============================================================================

SEP='  |  ' # separator between items
MAX_SONG=40 # characters of "artist - title" to keep

# Feature toggles
ENABLE_SONG=1
ENABLE_CPU=1
ENABLE_RAM=1
ENABLE_VOL=1
ENABLE_MIC=1
ENABLE_BAT=1
ENABLE_CLOCK=1
ENABLE_TEMP=1
ENABLE_WIFI=1
ENABLE_BT=1
ENABLE_POWER_PROFILE=1
TOGGLE_HOME=1
TOGGLE_BRIGHTNESS=1
TOGGLE_UPTIME=1

# Battery thresholds (for icon selection)
BAT_FULL_THRESHOLD=90
BAT_3_4_THRESHOLD=70
BAT_HALF_THRESHOLD=50
BAT_1_4_THRESHOLD=25

# Volume thresholds
VOL_LOW_THRESHOLD=50

# ============================================================================
# SYMBOL PLACEHOLDERS - CUSTOMIZE THESE
# ============================================================================

SYM_CPU=" "  # CPU symbol
SYM_RAM=" "  # RAM symbol
SYM_TEMP=" " # Temperature symbol

SYM_BAT_FULL="󰁹" # Battery full
SYM_BAT_HIGH="󰂁" # Battery 70-90%
SYM_BAT_HALF="󰁿" # Battery 50-70%
SYM_BAT_LOW="󰁻"  # Battery < 50%
SYM_CHARGING="󰂄" # Battery charging

SYM_VOL_MUTE=""  # Volume muted
SYM_VOL_LOW=" "  # Volume < 50%
SYM_VOL_HIGH=" " # Volume >= 50%

SYM_MIC_ON=""   # Microphone on
SYM_MIC_OFF=" " # Microphone off

SYM_MUSIC_PAUSE="" # Music paused
SYM_MUSIC_PLAY=""  # Music playing

SYM_POWER_BALANCED=" "    # Power profile: balanced
SYM_POWER_PERFORMANCE="󰡴 " # Power profile: performance
SYM_POWER_POWERSAVE="󰡳 "   # Power profile: power-saver

SYM_WIFI_4="󰤨 "   # 76-100%
SYM_WIFI_3="󰤥 "   # 51-75%
SYM_WIFI_2="󰤢 "   # 26-50%
SYM_WIFI_1="󰤟 "   # 1-25%
SYM_WIFI_OFF="󰤭 " # disconnected

SYM_BT="󰂱"     # Bluetooth on
SYM_BT_OFF="󰂲" # Bluetooth off (use a different glyph if you want)

SYM_HOME=" " # Home info

SYM_BRIGHTNESS="󰖨 "

SYM_UPTIME=""

# ============================================================================
# ONE-TIME SETUP
# ============================================================================

# CPU stats: keep previous sample for delta calculation
read -r _ u n s i w irq sirq st _ </proc/stat
prev_total=$((u + n + s + i + w + irq + sirq + st))
prev_idle=$((i + w))

# Battery: find first readable battery capacity
bat_cap=
for b in /sys/class/power_supply/BAT*; do
  if [ -r "$b/capacity" ]; then
    bat_cap="$b/capacity"
    break
  fi
done

# Thermal: find CPU temperature (optional)
temp_file=
if [ "$ENABLE_TEMP" -eq 1 ]; then
  for f in /sys/class/thermal/thermal_zone*/temp; do
    if [ -r "$f" ]; then
      temp_file="$f"
      break
    fi
  done
fi

# Wi-Fi: find an interface listed by the kernel
wlan_if=
if [ -r /proc/net/wireless ]; then
  wlan_if=$(awk 'NR > 2 { sub(/:/, "", $1); print $1; exit }' /proc/net/wireless)
fi

# Check for optional commands
have_playerctl=$(command -v playerctl >/dev/null 2>&1 && echo 1 || echo 0)
have_wpctl=$(command -v wpctl >/dev/null 2>&1 && echo 1 || echo 0)
have_powerprofilesctl=$(command -v powerprofilesctl >/dev/null 2>&1 && echo 1 || echo 0)

# ============================================================================
# HELPER FUNCTIONS
# ============================================================================

# Add item to output with separator
add() {
  [ -n "$1" ] || return
  if [ -n "$out" ]; then
    out="${out}${SEP}${1}"
  else
    out="$1"
  fi
}

sep() {
  [ -n "$1" ] || return
  out="${out}${1}"
}

# Safe file read (handles missing/unreadable files gracefully)
read_file() {
  local file="$1" default="$2"
  if [ -r "$file" ]; then
    read -r value <"$file"
    echo "$value"
  else
    echo "$default"
  fi
}

# Parse volume output: "Volume: 0.99" or "Volume: 0.99 [MUTED]"
parse_volume() {
  local vline="$1"
  vol_muted=0
  vol_pct=0

  case $vline in
  *'[MUTED]'*)
    vol_muted=1
    vline=${vline% \[MUTED\]}
    ;;
  esac

  local vfrac="${vline##* }"
  case $vfrac in
  1.00 | 1.0) vol_pct=100 ;;
  0.*)
    # Convert 0.XX to percentage
    vol_pct="${vfrac#0.}"
    vol_pct=$((vol_pct * 10))
    if [ ${#vfrac} -gt 4 ]; then
      vol_pct=$((vol_pct / 10))
    fi
    ;;
  *) vol_pct=0 ;;
  esac
}

# Parse microphone volume: "Volume: 0.80" or "Volume: 0.80 [MUTED]"
parse_mic_volume() {
  local mline="$1"
  mic_muted=0
  mic_pct=0

  case $mline in
  *'[MUTED]'*)
    mic_muted=1
    mline=${mline% \[MUTED\]}
    ;;
  esac

  local mfrac="${mline##* }"
  case $mfrac in
  1.00 | 1.0) mic_pct=100 ;;
  0.*)
    # Convert 0.XX to percentage
    mic_pct="${mfrac#0.}"
    mic_pct=$((mic_pct * 10))
    if [ ${#mfrac} -gt 4 ]; then
      mic_pct=$((mic_pct / 10))
    fi
    ;;
  *) mic_pct=0 ;;
  esac
}

# Get battery symbol based on percentage
get_battery_symbol() {
  local pct=$1
  if [ "$pct" -ge "$BAT_FULL_THRESHOLD" ]; then
    echo "$SYM_BAT_FULL"
  elif [ "$pct" -ge "$BAT_3_4_THRESHOLD" ]; then
    echo "$SYM_BAT_HIGH"
  elif [ "$pct" -ge "$BAT_HALF_THRESHOLD" ]; then
    echo "$SYM_BAT_HALF"
  else
    echo "$SYM_BAT_LOW"
  fi
}

# Get volume symbol based on level and mute status
get_volume_symbol() {
  local muted=$1 pct=$2
  if [ "$muted" -eq 1 ] || [ "$pct" -eq 0 ]; then
    echo "$SYM_VOL_MUTE"
  elif [ "$pct" -lt "$VOL_LOW_THRESHOLD" ]; then
    echo "$SYM_VOL_LOW"
  else
    echo "$SYM_VOL_HIGH"
  fi
}

# Get music symbol based on play/pause status
get_music_symbol() {
  local state=$1
  case $state in
  Playing) echo "$SYM_MUSIC_PLAY" ;;
  Paused) echo "$SYM_MUSIC_PAUSE" ;;
  *) echo "" ;;
  esac
}

# Get power profile symbol and name
get_power_profile() {
  if [ "$have_powerprofilesctl" -eq 1 ]; then
    local profile=$(powerprofilesctl get 2>/dev/null)
    case $profile in
    performance) echo "$SYM_POWER_PERFORMANCE" ;;
    balanced) echo "$SYM_POWER_BALANCED" ;;
    power-saver) echo "$SYM_POWER_POWERSAVE" ;;
    *) echo "" ;;
    esac
  fi
}

# get_home_storage() {
#   local home_data=$(df "$HOME" | awk 'NR==2 {print $3, $2}')
#   local home_usage=$(echo "$home_data" | awk '{printf "%d", $1/1024/1024}')
#   local home_total=$(echo "$home_data" | awk '{printf "%d", $2/1024/1024}')
#   printf "%dG/%dG" "$home_usage" "$home_total"
# }
# get_home_storage() {
#   df "$HOME" | awk 'NR==2 {printf "%d%%", $5}'
# }

get_home_storage() {
  df "$HOME" | awk 'NR==2 {printf "%dG", $3/1024/1024}'
}

get_brightness() {
  brightnessctl get | awk '{printf "%d%%", $1*100/max}' max=$(brightnessctl max)
}

get_uptime() {
  awk '{printf "%dh", $1/3600}' /proc/uptime
}

# ============================================================================
# MAIN LOOP
# ============================================================================

while :; do
  sleep 1
  out=

  # --- CPU USAGE ---
  if [ "$ENABLE_CPU" -eq 1 ]; then
    read -r _ u n s i w irq sirq st _ </proc/stat
    total=$((u + n + s + i + w + irq + sirq + st))
    idle=$((i + w))
    dt=$((total - prev_total))
    di=$((idle - prev_idle))

    if [ "$dt" -gt 0 ]; then
      cpu=$((100 * (dt - di) / dt))
    else
      cpu=0
    fi

    prev_total=$total
    prev_idle=$idle
    sep "  ${SYM_CPU} ${cpu}%"
  fi

  # --- RAM USAGE ---
  if [ "$ENABLE_RAM" -eq 1 ]; then
    mem_total=0
    mem_avail=0

    while read -r key val _; do
      case $key in
      MemTotal:) mem_total=$val ;;
      MemAvailable:)
        mem_avail=$val
        break
        ;;
      esac
    done </proc/meminfo

    if [ "$mem_total" -gt 0 ]; then
      ram=$((100 * (mem_total - mem_avail) / mem_total))
    else
      ram=0
    fi
    sep "  ${SYM_RAM} ${ram}%"
  fi

  if [ "$TOGGLE_HOME" = 1 ]; then
    home_storage=$(get_home_storage)
    add "${SYM_HOME} ${home_storage}"
  fi

  if [ "$TOGGLE_UPTIME" = 1 ]; then
    uptime=$(get_uptime)
    add "${SYM_UPTIME} ${uptime}"
  fi

  # --- WIFI ---
  if [ "${ENABLE_WIFI:-0}" -eq 1 ]; then
    if [ -n "$wlan_if" ] && [ -r /proc/net/wireless ]; then
      # /proc/net/wireless reports link quality in its third column.
      quality=$(awk -v iface="$wlan_if" '
        $1 == iface ":" { gsub(/\./, "", $3); print $3; exit }
      ' /proc/net/wireless)

      case $quality in
      '' | *[!0-9]*)
        add "$SYM_WIFI_OFF"
        ;;
      *)
        if [ "$quality" -ge 76 ]; then
          add "$SYM_WIFI_4"
        elif [ "$quality" -ge 51 ]; then
          add "$SYM_WIFI_3"
        elif [ "$quality" -ge 26 ]; then
          add "$SYM_WIFI_2"
        else
          add "$SYM_WIFI_1"
        fi
        ;;
      esac
    else
      add "$SYM_WIFI_OFF"
    fi
  fi

  # --- BLUETOOTH ---
  if [ "$ENABLE_BT" -eq 1 ]; then
    bt_state=$(bluetoothctl show 2>/dev/null | grep "Powered:" | awk '{print $2}')
    if [ "$bt_state" = "yes" ]; then
      sep "  $SYM_BT"
    else
      sep "  $SYM_BT_OFF Off"
    fi
  fi

  # --- TEMPERATURE (optional) ---
  if [ "$ENABLE_TEMP" -eq 1 ] && [ -n "$temp_file" ]; then
    temp_raw=$(read_file "$temp_file" "0")
    # Convert from millidegrees to degrees
    temp=$((temp_raw / 1000))
    add "${SYM_TEMP} ${temp}°C"
  fi
  # if [ "$ENABLE_TEMP" -eq 1 ]; then
  #   temp_file=$(for f in /sys/class/thermal/thermal_zone*/temp; do
  #     [ -r "$f" ] && echo "$f" && break
  #   done)
  #   if [ -n "$temp_file" ]; then
  #     temp_raw=$(read_file "$temp_file" "0")
  #     temp=$((temp_raw / 1000))
  #     add "${SYM_TEMP} ${temp}°C"
  #   fi
  # fi

  # --- VOLUME & MICROPHONE ---
  if [ "$ENABLE_VOL" -eq 1 ] || [ "$ENABLE_MIC" -eq 1 ]; then
    if [ "$have_wpctl" -eq 1 ]; then
      # Volume
      if [ "$ENABLE_VOL" -eq 1 ]; then
        vline=$(wpctl get-volume @DEFAULT_AUDIO_SINK@ 2>/dev/null)
        if [ -n "$vline" ]; then
          parse_volume "$vline"
          vol_symbol=$(get_volume_symbol "$vol_muted" "$vol_pct")
          if [ "$vol_muted" -eq 1 ] || [ "$vol_pct" -eq 0 ]; then
            add "${vol_symbol} Mute"
          else
            add "${vol_symbol} ${vol_pct}%"
          fi
        fi
      fi

      # Microphone - WITH PERCENTAGE
      if [ "$ENABLE_MIC" -eq 1 ]; then
        mline=$(wpctl get-volume @DEFAULT_AUDIO_SOURCE@ 2>/dev/null)
        if [ -n "$mline" ]; then
          parse_mic_volume "$mline"
          if [ "$mic_muted" -eq 1 ]; then
            sep "  ${SYM_MIC_OFF} Mute"
          else
            sep "  ${SYM_MIC_ON} ${mic_pct}%"
          fi
        fi
      fi
    fi
  fi

  # --- BATTERY ---
  if [ "$ENABLE_BAT" -eq 1 ] && [ -n "$bat_cap" ]; then
    pct=$(read_file "$bat_cap" "")

    if [ -n "$pct" ] && echo "$pct" | grep -q '^[0-9]*$'; then
      state=$(read_file "${bat_cap%/capacity}/status" "Unknown")

      if [ "$state" = "Charging" ]; then
        add "${SYM_CHARGING} ${pct}%"
      else
        bat_symbol=$(get_battery_symbol "$pct")
        add "${bat_symbol} ${pct}%"
      fi
    fi
  fi

  # --- POWER PROFILE ---
  if [ "${ENABLE_POWER_PROFILE:-0}" -eq 1 ]; then
    power_symbol=$(get_power_profile)
    if [ -n "$power_symbol" ]; then
      sep "  $power_symbol"
    fi
  fi

  if [ "$TOGGLE_BRIGHTNESS" = 1 ]; then
    brightness=$(get_brightness)
    sep " ${SYM_BRIGHTNESS} ${brightness}"
  fi

  # --- MUSIC (playerctl) - WITH PLAY/PAUSE SYMBOLS ---
  if [ "$ENABLE_SONG" -eq 1 ] && [ "$have_playerctl" -eq 1 ]; then
    line=$(timeout 0.5 playerctl metadata \
      --format '{{status}}|{{artist}} - {{title}}' 2>/dev/null)
    state="${line%%|*}"

    case $state in
    Playing | Paused)
      song="${line#*|}"
      [ "$song" = " - " ] && song=
      if [ -n "$song" ]; then
        song=$(printf '%s' "$song" | cut -c1-"$MAX_SONG")
      fi
      music_symbol=$(get_music_symbol "$state")
      if [ -n "$song" ]; then
        add "${music_symbol} ${song}"
      elif [ -n "$music_symbol" ]; then
        add "$music_symbol"
      fi
      ;;
    esac
  fi

  # --- CLOCK (always last, so it doesn't get truncated) ---
  if [ "$ENABLE_CLOCK" -eq 1 ]; then
    add "$(date '+%a %b %d %H:%M')  "
  fi

  printf '%s\n' "$out"
done
