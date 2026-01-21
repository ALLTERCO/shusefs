# Shelly FUSE Filesystem (shusefs)

A FUSE-based filesystem for managing Shelly Gen2+ IoT devices through standard file operations.

## Rationale

The idea is to provide a method to "mount" a Shelly Gen2+ device on a host system, exposing device configuration and scripts as regular files. This enables standard file manipulation methods to change device configuration and edit scripts, providing a maintenance approach that:

- Aligns with standard system administration practices
- Uses familiar tools (vi, cat, sed, etc.)
- Limits exposure to specialized tools and API knowledge
- Enables automation through standard shell scripting

## User Persona

System administrator or reliability engineer on a Linux system who can use simple tools and scripting to maintain IoT devices without learning device-specific APIs.

## Technology

- **Language**: Standard POSIX C code
- **WebSocket**: Mongoose library for JSON-RPC communication
- **Filesystem**: FUSE3 for filesystem operations
- **Protocol**: Shelly Gen2+ JSON-RPC over WebSocket

## Prerequisites

- FUSE library:
  - Linux: FUSE3 (`libfuse3-dev` on Debian/Ubuntu, `fuse3-devel` on Fedora/RHEL)
  - macOS: macFUSE or FUSE-T
- mongoose websocket library (included in source)
- GCC or Clang C compiler
- make
- pkg-config

### Installing Dependencies

**Debian/Ubuntu:**
```bash
sudo apt-get install libfuse3-dev build-essential pkg-config
```

**Fedora/RHEL:**
```bash
sudo dnf install fuse3-devel gcc make pkg-config
```

**macOS (with Homebrew):**
```bash
# Option 1: macFUSE (kernel extension based, well-established)
brew install macfuse

# Option 2: FUSE-T (NFSv4 based, no kernel extension required)
brew install fuse-t
```

Alternatively, download macFUSE from [macfuse.github.io](https://macfuse.github.io/).

**Note:** On macOS, after installing macFUSE, you may need to allow the kernel extension in System Settings > Privacy & Security.

## Building

```bash
make
```

This creates the `shusefs` executable in the project directory.

The Makefile automatically detects the platform and configures the build appropriately:
- **Linux**: Uses FUSE3 (libfuse3) with `gcc`
- **macOS**: Uses macFUSE/FUSE-T with `clang`

To see the detected build configuration:
```bash
make info
```

## Installation

```bash
sudo make install
```

This installs the binary to `/usr/local/bin/shusefs`.

## Usage

### Basic Usage

Mount a Shelly device to a local directory:

```bash
shusefs <device_websocket_url> <mount_point>
```

Example:
```bash
mkdir /tmp/shelly
shusefs ws://192.168.1.100:80/rpc /tmp/shelly
```

To unmount:
```bash
# Linux
fusermount3 -u /tmp/shelly

# macOS
umount /tmp/shelly

# Or press Ctrl+C in the terminal running shusefs
```

### File Structure

After mounting, the device appears as a filesystem:

```
/tmp/shelly/
├── sys_config.json           # System configuration (read-write)
├── mqtt_config.json          # MQTT configuration (read-write)
├── switch_0_config.json      # Switch 0 configuration (read-write)
├── switch_1_config.json      # Switch 1 configuration (if present)
├── ...                       # Additional switches (up to 16)
├── input_0_config.json       # Input 0 configuration (read-write)
├── input_1_config.json       # Input 1 configuration (if present)
├── ...                       # Additional inputs (up to 16)
├── crontab                   # Schedule management (read-write, cron-like format)
├── scripts/                  # Scripts directory
│   ├── script_1.js           # Script files (read-write)
│   ├── script_2.js
│   └── ...                   # Up to 10 scripts
└── proc/                     # Real-time control and monitoring
    ├── switch/               # Switch control and status
        ├── 0/
        │   ├── output        # Switch 0 on/off (read-write, immediate)
        │   ├── apower        # Active power in watts (read-only, real-time)
        │   ├── voltage       # Voltage in volts (read-only, real-time)
        │   ├── current       # Current in amperes (read-only, real-time)
        │   ├── temperature   # Temperature in °C (read-only, real-time)
        │   ├── energy        # Total energy in Wh (read-only, real-time)
        │   ├── ret_energy    # Returned energy in Wh (read-only, real-time)
        │   ├── freq          # AC frequency in Hz (read-only, real-time)
        │   ├── id            # Switch ID (read-only)
        │   └── source        # Power source (read-only)
        ├── 1/
        │   └── ...           # Switch 1 files (if present)
        └── ...               # Additional switches
    └── input/                # Input monitoring
        ├── 0/
        │   ├── id            # Input ID (read-only)
        │   └── state         # Input state true/false (read-only, real-time)
        ├── 1/
        │   └── ...           # Input 1 files (if present)
        └── ...               # Additional inputs
```

## Features

### 1. Configuration Management (Bidirectional Sync)

Configuration files support **full read-write access** with automatic synchronization:

#### System Configuration (`sys_config.json`)
- Contains device name, location, timezone, eco mode, debug settings
- **Read**: `cat /tmp/shelly/sys_config.json`
- **Edit**: `vi /tmp/shelly/sys_config.json` (or any text editor)
- **Changes**: Automatically sent to device on save
- **Auto-refresh**: Updates from device changes appear in the file

#### MQTT Configuration (`mqtt_config.json`)
- Contains MQTT broker settings, credentials, topic prefix, SSL options
- **Read**: `cat /tmp/shelly/mqtt_config.json`
- **Edit**: `vi /tmp/shelly/mqtt_config.json`
- **Changes**: Automatically sent to device on save
- **Auto-refresh**: Updates from device changes appear in the file

#### Switch Configuration (`switch_N_config.json`)
- One file per switch (switch_0_config.json, switch_1_config.json, etc.)
- Contains input mode, initial state, auto-on/off timers, power limits
- **Read**: `cat /tmp/shelly/switch_0_config.json`
- **Edit**: `vi /tmp/shelly/switch_0_config.json`
- **Changes**: Automatically sent to device on save
- **Auto-refresh**: Updates from device changes appear in the file

#### Input Configuration (`input_N_config.json`)
- One file per input (input_0_config.json, input_1_config.json, etc.)
- Contains input type (switch/button/analog), enable state, invert flag, factory_reset settings
- **Read**: `cat /tmp/shelly/input_0_config.json`
- **Edit**: `vi /tmp/shelly/input_0_config.json`
- **Changes**: Automatically sent to device on save
- **Auto-refresh**: Updates from device changes appear in the file

### 2. Script Editing (Bidirectional Sync)

Scripts support **full read-write access** with automatic upload:

- **List scripts**: `ls /tmp/shelly/scripts/`
- **Read script**: `cat /tmp/shelly/scripts/script_5.js`
- **Edit script**: `vi /tmp/shelly/scripts/script_5.js`
- **Changes**: Automatically uploaded to device on save (in 2048-byte chunks)
- **Persistence**: Scripts persist on device across reboots

### 3. Real-time Switch Control and Monitoring (Proc Filesystem)

The `/proc` directory provides **immediate device control and real-time monitoring** through simple file operations:

#### Control (Read/Write)
- **output**: Turn switches on/off with instant action
  - Write: `echo true > /tmp/shelly/proc/switch/0/output`
  - Read: `cat /tmp/shelly/proc/switch/0/output`
  - Values: "true"/"false" or "1"/"0"
  - No buffering - changes happen immediately

#### Status Monitoring (Read-Only, Real-Time)
- **apower**: Active power consumption in watts
- **voltage**: Line voltage in volts
- **current**: Current draw in amperes
- **temperature**: Device temperature in Celsius
- **energy**: Total energy consumed in watt-hours
- **ret_energy**: Total returned energy in watt-hours (if available)
- **freq**: AC frequency in Hz (if available)
- **id**: Switch ID number
- **source**: Power source indicator (e.g., "WS_in", "init")

All status files update automatically via WebSocket notifications from the device, providing real-time values without polling.

### 4. Real-time Input Monitoring (Proc Filesystem)

The `/proc/input` directory provides **real-time monitoring** of device inputs through simple file operations:

#### Status Monitoring (Read-Only, Real-Time)
- **id**: Input ID number
- **state**: Input state (true/false)
  - Read: `cat /tmp/shelly/proc/input/0/state`
  - Values: "true" or "false"
  - Updates automatically via WebSocket notifications

All input status files update automatically via WebSocket notifications from the device, providing real-time values without polling.

### 5. Schedule Management (Crontab)

The `/crontab` file provides a familiar cron-like interface for managing device schedules:

- **Read schedules**: `cat /tmp/shelly/crontab`
- **Edit schedules**: `vi /tmp/shelly/crontab`
- **Add schedules**: Append new lines to the file
- **Delete schedules**: Remove lines from the file
- **Disable schedules**: Prefix lines with `#!`
- **Re-enable schedules**: Remove the `#!` prefix

#### Crontab Format

Shelly uses a **6-field timespec** (unlike standard cron's 5 fields):

```
sec min hour dom month dow method params
```

Fields:
- `sec` - Seconds (0-59)
- `min` - Minutes (0-59)
- `hour` - Hour (0-23)
- `dom` - Day of month (1-31)
- `month` - Month (1-12)
- `dow` - Day of week (0-6, 0=Sunday)
- `method` - Shelly RPC method to call (e.g., `Switch.Set`)
- `params` - JSON parameters for the method

Special lines:
- `# id:N` - Comment identifying the schedule ID (auto-generated)
- `#!` prefix - Disabled schedule (will not run)

#### Example Crontab

```crontab
# id:1
0 0 6 * * 0,1,2,3,4,5,6 Switch.Set {"id":0,"on":true}

# id:2 (disabled)
#! 0 30 22 * * * Switch.Set {"id":0,"on":false}
```

### 6. Per-Field mtime Tracking and inotify Support

Each proc file maintains **independent modification times** (mtime) that update only when that specific value changes:

- **Precise change detection**: Monitor only the values you care about
- **inotify-compatible**: Standard file monitoring tools can detect changes
- **Efficient workflows**: No need to poll all files - watch specific metrics
- **Timestamp accuracy**: mtime reflects when the device last reported a change

Example - Monitor power consumption changes:
```python
import os, time
last_mtime = os.stat("testmnt/proc/switch/0/apower").st_mtime
while True:
    current_mtime = os.stat("testmnt/proc/switch/0/apower").st_mtime
    if current_mtime != last_mtime:
        with open("testmnt/proc/switch/0/apower") as f:
            power = float(f.read())
        print(f"Power changed: {power}W")
        last_mtime = current_mtime
    time.sleep(1)
```

### 7. Automatic Notification Handling

- **WebSocket notifications**: Device sends real-time status updates
- **Automatic updates**: File contents reflect device state immediately
- **Bidirectional sync**: Changes from web UI, MQTT, or buttons appear instantly
- **Connection resilience**: Automatic reconnection on connection loss

## How Configuration Handling Works

### Bidirectional Synchronization

shusefs implements **true bidirectional sync** for all configuration files:

#### User → Device (Write Path)

1. **User edits file**: Opens config file in any text editor
2. **FUSE write buffer**: Changes accumulate in memory during editing
3. **Flush on close**: When file is saved/closed, FUSE flush handler is called
4. **JSON validation**: Config JSON is validated before sending
5. **Send to device**: Config sent via appropriate RPC method:
   - `Sys.SetConfig` for sys_config.json
   - `MQTT.SetConfig` for mqtt_config.json
   - `Switch.SetConfig` for switch_N_config.json
6. **Wait for response**: Request queued and sent over WebSocket
7. **Device applies**: Device validates and applies the configuration
8. **Auto-refresh triggered**: On success response, filesystem requests fresh config
9. **Canonical state**: Device returns its current config (what it actually applied)
10. **File updated**: Filesystem updates the file with canonical device state

This ensures the file always reflects what the device actually has, not just what the user wrote.

#### Device → User (Notification Path)

1. **Device changes**: Configuration changed externally (web UI, MQTT, other client)
2. **Notification sent**: Device sends `NotifyEvent` notification via WebSocket
3. **Notification detected**: Filesystem recognizes config_changed event
4. **Request fresh config**: Filesystem requests current config from device
5. **Device responds**: Device sends current configuration
6. **File updated**: Filesystem updates the file content
7. **User sees change**: Next read shows the updated configuration

### Configuration File Format

All configuration files use **JSON format** and follow the device's native schema:

#### sys_config.json Example
```json
{
  "device": {
    "name": "Living Room Light",
    "mac": "A4CF1234ABCD",
    "fw_id": "20250709-190643/g350c2c9",
    "discoverable": true,
    "eco_mode": false
  },
  "location": {
    "tz": "America/New_York",
    "lat": 40.7128,
    "lon": -74.0060
  },
  "debug": {
    "level": 3,
    "mqtt": {"enable": false},
    "websocket": {"enable": true}
  },
  "sntp": {
    "server": "time.google.com"
  }
}
```

#### mqtt_config.json Example
```json
{
  "enable": true,
  "server": "mqtt.example.com:1883",
  "client_id": "shelly-livingroom",
  "user": "mqtt_user",
  "topic_prefix": "shellies/livingroom",
  "ssl_ca": "user_ca",
  "enable_control": true,
  "enable_rpc": true,
  "rpc_ntf": true,
  "status_ntf": true
}
```

#### switch_0_config.json Example
```json
{
  "id": 0,
  "name": "Main Switch",
  "in_mode": "follow",
  "initial_state": "restore_last",
  "auto_on": false,
  "auto_on_delay": 60.0,
  "auto_off": true,
  "auto_off_delay": 600.0,
  "power_limit": 4480,
  "voltage_limit": 280,
  "current_limit": 16.0
}
```

### Error Handling

- **Invalid JSON**: Error returned to filesystem, original content preserved
- **Device rejection**: Device error reported, original content preserved
- **Connection loss**: Changes queued, sent when connection restored
- **Concurrent edits**: Last write wins (similar to file editing conventions)

## How Script Editing Works

### Script Upload Process

1. **User edits script**: Opens script file (e.g., `vi /tmp/shelly/scripts/script_5.js`)
2. **FUSE write buffer**: Changes accumulate in memory during editing
3. **Flush on close**: When file is saved/closed, FUSE flush handler triggered
4. **Chunked upload**: Script sent to device in chunks:
   - Chunk size: 2048 bytes
   - First chunk: `Script.PutCode` with `append: false`
   - Subsequent chunks: `Script.PutCode` with `append: true`
   - JSON-escaped code content
5. **Device receives**: Device assembles chunks and saves script
6. **Local update**: Filesystem updates local cache with new content
7. **Persistence**: Script persists on device across reboots

### Script File Format

Scripts are plain JavaScript files following Shelly's mJS dialect:

```javascript
// Example script
let config = {
  threshold: 25.0,
  interval: 60000
};

function checkTemperature() {
  Shelly.call("Temperature.GetStatus", {},
    function(result, error_code, error_message) {
      if (error_code === 0) {
        let temp = result.tC;
        if (temp > config.threshold) {
          print("Temperature high:", temp);
          // Take action
        }
      }
    }
  );
}

// Run every minute
Timer.set(config.interval, true, checkTemperature);
```

### Script Limitations

- **Maximum scripts**: 10 scripts per device (script_1.js through script_10.js)
- **Maximum size**: 20KB per script (MAX_SCRIPT_CODE = 20480 bytes)
- **Chunk size**: 2048 bytes per upload chunk
- **Language**: mJS (JavaScript subset) - see Shelly documentation
- **Special characters**: Automatically JSON-escaped during upload

## How Proc Filesystem Works

### Real-time Device Control and Monitoring

The `/proc` directory provides a Unix-like interface for **immediate device control and real-time monitoring**, similar to `/proc` on Linux systems. Unlike configuration files, proc files trigger instant actions and display live device data.

#### User → Device (Control Path)

1. **User writes output**: `echo true > /proc/switch/0/output`
2. **Immediate action**: FUSE write handler called immediately (no buffering)
3. **Parse value**: "true"/"1" → turn on, "false"/"0" → turn off
4. **Send command**: `Switch.Set` JSON-RPC call sent to device
5. **Device responds**: Device changes switch state and returns status
6. **Update local state**: All status fields updated from response (output, apower, voltage, etc.)
7. **Update mtime**: Modified files get new timestamps
8. **Verify state**: Automatic `Switch.GetStatus` request for confirmation

**Key difference from config files**: Action happens on write, not on file close/flush.

#### Device → User (Status Update Path - Notifications)

1. **Device state changes**: Any status value changes (switch toggled, power changes, temperature varies)
2. **Notification sent**: Device sends `NotifyStatus` notification via WebSocket with updated values
3. **Notification detected**: Filesystem parses notification for switch status updates
4. **Parse status fields**: Extract all changed values from notification:
   - output (on/off state)
   - apower (power consumption)
   - voltage, current, freq
   - temperature
   - energy totals
   - source (power source)
5. **Update only changed fields**: Compare new values with cached values
6. **Update mtimes**: Set per-field mtime for only the changed values
7. **User reads**: Next read shows updated value, stat shows changed mtime

**Real-time updates**: Status files reflect device state within milliseconds of changes.

### Proc File Format

Proc files use **simple text format** for easy shell scripting:

#### Switch Output File (`/proc/switch/N/output`)

**Read** - Returns current on/off state:
```
true
```
or
```
false
```

**Write** - Accepts "true"/"false" or "1"/"0":
```bash
echo true > /proc/switch/0/output   # Turn on
echo false > /proc/switch/0/output  # Turn off
echo 1 > /proc/switch/0/output      # Turn on (alternative)
echo 0 > /proc/switch/0/output      # Turn off (alternative)
```

#### Status Files (Read-Only)

All status files return numeric values or strings in plain text:

**apower** - Active power in watts:
```bash
$ cat /proc/switch/0/apower
5.1
```

**voltage** - Line voltage in volts:
```bash
$ cat /proc/switch/0/voltage
230.4
```

**temperature** - Device temperature in Celsius:
```bash
$ cat /proc/switch/0/temperature
48.3
```

**energy** - Total consumed energy in watt-hours:
```bash
$ cat /proc/switch/0/energy
10.245
```

### Status Tracking

Each switch tracks real-time status with per-field timestamps:
- **output** - Current on/off state (read-write, with mtime)
- **apower** - Active power consumption in watts (read-only, with mtime)
- **voltage** - Line voltage in volts (read-only, with mtime)
- **current** - Current draw in amperes (read-only, with mtime)
- **temperature** - Device temperature in Celsius (read-only, with mtime)
- **energy** - Total energy consumed in Wh (read-only, with mtime)
- **ret_energy** - Total returned energy in Wh (read-only, with mtime)
- **freq** - AC frequency in Hz (read-only, with mtime)
- **id** - Switch ID number (read-only, with mtime)
- **source** - Power source indicator (read-only, with mtime)

Each file's mtime updates independently when that specific value changes, enabling precise monitoring.

### Error Handling

- **Invalid switch ID**: FUSE returns "No such file or directory"
- **Invalid value**: Write ignored, warning logged
- **Device error**: Error logged, state file unchanged
- **Connection loss**: Commands queued, sent when connection restored

## Examples

### Control Switches

```bash
# Turn switch 0 ON
echo true > /tmp/shelly/proc/switch/0/output
# or
echo 1 > /tmp/shelly/proc/switch/0/output

# Turn switch 0 OFF
echo false > /tmp/shelly/proc/switch/0/output
# or
echo 0 > /tmp/shelly/proc/switch/0/output

# Check current state
cat /tmp/shelly/proc/switch/0/output

# Toggle switch state
current=$(cat /tmp/shelly/proc/switch/0/output)
if [ "$current" = "true" ]; then
  echo false > /tmp/shelly/proc/switch/0/output
else
  echo true > /tmp/shelly/proc/switch/0/output
fi
```

### Monitor Real-Time Status

```bash
# Read power consumption
cat /tmp/shelly/proc/switch/0/apower
# Output: 5.1

# Read voltage
cat /tmp/shelly/proc/switch/0/voltage
# Output: 230.4

# Read temperature
cat /tmp/shelly/proc/switch/0/temperature
# Output: 48.3

# Read all status values
echo "Switch Status:"
echo "  Output: $(cat /tmp/shelly/proc/switch/0/output)"
echo "  Power: $(cat /tmp/shelly/proc/switch/0/apower)W"
echo "  Voltage: $(cat /tmp/shelly/proc/switch/0/voltage)V"
echo "  Current: $(cat /tmp/shelly/proc/switch/0/current)A"
echo "  Temperature: $(cat /tmp/shelly/proc/switch/0/temperature)°C"
echo "  Energy: $(cat /tmp/shelly/proc/switch/0/energy)Wh"
```

### Monitor Power Consumption Changes

```bash
#!/bin/bash
# monitor-power.sh - Watch for power consumption changes

POWER_FILE="/tmp/shelly/proc/switch/0/apower"
last_mtime=$(stat -c %Y "$POWER_FILE")

while true; do
  current_mtime=$(stat -c %Y "$POWER_FILE")
  if [ "$current_mtime" != "$last_mtime" ]; then
    power=$(cat "$POWER_FILE")
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] Power changed to: ${power}W"
    last_mtime=$current_mtime
  fi
  sleep 1
done
```

### High Power Alert

```bash
#!/bin/bash
# power-alert.sh - Alert when power exceeds threshold

THRESHOLD=10.0
POWER_FILE="/tmp/shelly/proc/switch/0/apower"

while true; do
  power=$(cat "$POWER_FILE")
  if (( $(echo "$power > $THRESHOLD" | bc -l) )); then
    echo "ALERT: High power consumption: ${power}W"
    # Send notification, trigger action, etc.
  fi
  sleep 5
done
```

### Automated Switch Control

```bash
#!/bin/bash
# auto-lights.sh - Turn lights on at sunset, off at sunrise

LIGHT_SWITCH="/tmp/shelly/proc/switch/0/output"

# Turn on lights
echo true > $LIGHT_SWITCH
echo "Lights turned ON at $(date)"

# Schedule turn off for 8 hours later
(sleep 28800 && echo false > $LIGHT_SWITCH && echo "Lights turned OFF at $(date)") &
```

### Monitor Switch State Changes

```bash
#!/bin/bash
# monitor-switch.sh - Watch for switch state changes using mtime

SWITCH="/tmp/shelly/proc/switch/0/output"
last_mtime=$(stat -c %Y "$SWITCH")

while true; do
  current_mtime=$(stat -c %Y "$SWITCH")
  if [ "$current_mtime" != "$last_mtime" ]; then
    state=$(cat "$SWITCH")
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] Switch changed to: $state"
    last_mtime=$current_mtime
  fi
  sleep 1
done
```

### Change Device Name

```bash
# Read current config
cat /tmp/shelly/sys_config.json | jq .

# Edit device name
jq '.device.name = "Kitchen Light"' /tmp/shelly/sys_config.json > /tmp/temp.json
cat /tmp/temp.json > /tmp/shelly/sys_config.json

# Verify change
sleep 2
cat /tmp/shelly/sys_config.json | jq .device.name
```

### Configure MQTT Broker

```bash
# Edit MQTT settings
vi /tmp/shelly/mqtt_config.json
# Change "server" field, save and exit

# Changes are automatically sent to device
# Device will reconnect to new broker
```

### Toggle Switch Auto-Off

```bash
# Read current switch config
cat /tmp/shelly/switch_0_config.json | jq .

# Toggle auto_off
jq '.auto_off = true | .auto_off_delay = 300.0' /tmp/shelly/switch_0_config.json > /tmp/temp.json
cat /tmp/temp.json > /tmp/shelly/switch_0_config.json

# Verify change
cat /tmp/shelly/switch_0_config.json | jq .auto_off
```

### Edit a Script

```bash
# Create/edit a script
cat > /tmp/shelly/scripts/script_5.js << 'EOF'
// Temperature monitoring script
let threshold = 30.0;

function checkTemp() {
  Shelly.call("Temperature.GetStatus", {},
    function(result, error_code, error_message) {
      if (error_code === 0 && result.tC > threshold) {
        print("Temperature alert:", result.tC, "°C");
        // Send notification or trigger action
      }
    }
  );
}

Timer.set(60000, true, checkTemp);
EOF

# Script is automatically uploaded to device in chunks
# Verify upload completed by checking logs
```

### Manage Schedules (Crontab)

```bash
# View current schedules
cat /tmp/shelly/crontab

# Example output:
# # id:1
# 0 0 6 * * 0,1,2,3,4,5,6 Switch.Set {"id":0,"on":true}

# Add a new schedule (turn on switch at 7:30 AM every day)
echo '0 30 7 * * * Switch.Set {"id":0,"on":true}' >> /tmp/shelly/crontab

# Create a schedule to turn off switch at 10 PM on weekdays
echo '0 0 22 * * 1,2,3,4,5 Switch.Set {"id":0,"on":false}' >> /tmp/shelly/crontab

# Schedule a device reboot every Sunday at 3 AM
echo '0 0 3 * * 0 Shelly.Reboot {}' >> /tmp/shelly/crontab

# Disable a schedule (prefix with #!)
# Edit the crontab and change:
#   0 0 6 * * * Switch.Set {"id":0,"on":true}
# to:
#   #! 0 0 6 * * * Switch.Set {"id":0,"on":true}

# Re-enable a schedule (remove #! prefix)
# Edit the crontab and remove the #! prefix

# Delete all schedules
echo -n > /tmp/shelly/crontab

# Replace all schedules with new ones
cat > /tmp/shelly/crontab << 'EOF'
0 0 7 * * 1,2,3,4,5 Switch.Set {"id":0,"on":true}
0 0 22 * * 1,2,3,4,5 Switch.Set {"id":0,"on":false}
EOF
```

### Backup All Configurations

```bash
# Create backup directory
mkdir -p shelly-backup

# Copy all configs, scripts, and schedules
cp /tmp/shelly/sys_config.json shelly-backup/
cp /tmp/shelly/mqtt_config.json shelly-backup/
cp /tmp/shelly/switch_*.json shelly-backup/ 2>/dev/null
cp /tmp/shelly/crontab shelly-backup/
cp -r /tmp/shelly/scripts shelly-backup/

# Backup complete
tar czf shelly-backup-$(date +%Y%m%d).tar.gz shelly-backup/
```

### Restore Configuration

```bash
# Restore from backup
cat shelly-backup/mqtt_config.json > /tmp/shelly/mqtt_config.json
cat shelly-backup/sys_config.json > /tmp/shelly/sys_config.json
cat shelly-backup/crontab > /tmp/shelly/crontab

# Restore scripts
for script in shelly-backup/scripts/*.js; do
  cp "$script" /tmp/shelly/scripts/
done

# All changes automatically synced to device
```

### Automated Configuration Management

```bash
#!/bin/bash
# configure-shelly.sh - Automated Shelly configuration

MOUNT="/tmp/shelly"

# Set device name and location
jq '.device.name = "Living Room" | .location.tz = "America/New_York"' \
  $MOUNT/sys_config.json > /tmp/temp.json && \
  cat /tmp/temp.json > $MOUNT/sys_config.json

# Configure MQTT
jq '.enable = true | .server = "mqtt.home:1883" | .user = "shelly"' \
  $MOUNT/mqtt_config.json > /tmp/temp.json && \
  cat /tmp/temp.json > $MOUNT/mqtt_config.json

# Configure auto-off for switch 0
jq '.auto_off = true | .auto_off_delay = 600.0' \
  $MOUNT/switch_0_config.json > /tmp/temp.json && \
  cat /tmp/temp.json > $MOUNT/switch_0_config.json

echo "Configuration applied successfully"
```

## Architecture

### Core Components

1. **FUSE Operations** (`fuse_ops.c`)
   - Implements FUSE callbacks (getattr, read, write, flush, etc.)
   - Manages file handles and write buffers
   - Routes operations to device state manager

2. **Device State Manager** (`device_state.c`)
   - Maintains local cache of device state
   - Handles JSON-RPC request/response flow
   - Manages chunked data transfers
   - Implements bidirectional synchronization logic

3. **Request Queue** (`request_queue.c`)
   - Queues outgoing JSON-RPC requests
   - Matches responses to requests
   - Manages request IDs and timeouts

4. **WebSocket Handler** (`main.c`)
   - Maintains WebSocket connection to device
   - Sends queued requests
   - Receives and dispatches responses
   - Handles notifications

### Data Flow

```
User Edit → FUSE Write → Write Buffer → FUSE Flush →
Device State → JSON-RPC Request → Request Queue →
WebSocket → Device

Device Response → WebSocket → Response Handler →
Device State Update → Refresh Request → Device →
Updated State → File Content

Device Notification → WebSocket → Notification Handler →
Refresh Request → Device → Updated State → File Content
```

## Troubleshooting

### FUSE mount fails
```bash
# Check if FUSE is available
ls /dev/fuse

# Check if user has permissions
groups | grep fuse

# Try with explicit permissions
shusefs ws://192.168.1.100:80/rpc /tmp/shelly -o allow_other
```

### Changes not appearing
```bash
# Check connection status
# Look for "WebSocket connection established" in logs

# Verify file permissions
ls -la /tmp/shelly/

# Check device accessibility
ping 192.168.1.100
```

### Device not responding
```bash
# Check device is reachable
curl http://192.168.1.100/rpc/Shelly.GetDeviceInfo

# Verify WebSocket endpoint
wscat -c ws://192.168.1.100:80/rpc

# Restart filesystem
fusermount3 -u /tmp/shelly
shusefs ws://192.168.1.100:80/rpc /tmp/shelly
```

## Limitations

- **Single connection**: One filesystem mount per device recommended
- **No file deletion**: Scripts cannot be deleted via filesystem (use device API)
- **No directory creation**: Directory structure is fixed
- **Text-based editing**: Binary file operations not supported
- **Device capabilities**: Some devices may not have all features (switches, scripts)

## Contributing

Contributions welcome! Please ensure:
- Code follows existing style
- Changes compile without warnings
- Features are tested with real Shelly devices

## License

Copyright 2025 Shelly Europe SE

Licensed under the Apache License, Version 2.0. See the [LICENSE](LICENSE) file for details.

## References

- [Shelly Gen2+ API Documentation](https://shelly-api-docs.shelly.cloud/)
- [FUSE Documentation](https://www.kernel.org/doc/html/latest/filesystems/fuse.html)
- [Mongoose WebSocket Library](https://mongoose.ws/)
