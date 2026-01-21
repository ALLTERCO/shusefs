# CLAUDE.md - Project Context for Claude Code

## Project Overview

**shusefs** (Shelly FUSE Filesystem) is a FUSE-based filesystem that mounts Shelly Gen2+ IoT devices as standard Linux/Unix directories. It enables device management through familiar file operations and shell scripting instead of device-specific APIs.

## Build Commands

```bash
make              # Build the executable
make install      # Install to /usr/local/bin/shusefs
make clean        # Remove build artifacts
make format       # Run clang-format on source files
make check-format # Verify formatting without changes
make tidy         # Run clang-tidy static analysis
```

**Dependencies:**
- Linux: `libfuse3-dev`
- macOS: `macfuse` or `fuse-t`

## Architecture

```
┌─────────────┐     ┌──────────────────┐     ┌──────────────┐
│ FUSE Thread │────▶│  Device State    │◀────│ Main Thread  │
│ (fuse_ops)  │     │  (device_state)  │     │ (WebSocket)  │
└─────────────┘     └──────────────────┘     └──────────────┘
                            │
                    ┌───────┴───────┐
                    │ Request Queue │
                    └───────────────┘
```

- **Main Thread**: WebSocket event loop communicating with Shelly device
- **FUSE Thread**: Handles filesystem operations from user
- **Device State**: Synchronized cache of device configuration and status
- **Request Queue**: Manages async JSON-RPC request/response matching

## Key Files

| File | Purpose |
|------|---------|
| `src/main.c` | Entry point, WebSocket handler, event loop |
| `src/device_state.c` | Device state manager, JSON parsing, config sync |
| `src/fuse_ops.c` | FUSE filesystem operations (read/write/readdir) |
| `src/request_queue.c` | Async request queue with 30s timeout |
| `src/mongoose.c` | Bundled WebSocket library (do not modify) |

## Code Conventions

- C11 standard
- Use `clang-format` for formatting (config in `clang-format` file)
- Use `clang-tidy` for static analysis (config in `clang-tidy` file)
- Single mutex per major data structure for thread safety
- JSON-RPC 2.0 protocol for device communication

## Device State Structure

The filesystem exposes:
- `/sys_config.json` - System configuration
- `/mqtt_config.json` - MQTT configuration
- `/switch_N_config.json` - Switch configurations (N=0-15)
- `/input_N_config.json` - Input configurations (N=0-15)
- `/scripts/script_N.js` - JavaScript scripts (N=0-9, max 20KB)
- `/crontab` - Schedules in crontab format
- `/proc/switch/N/{output,apower,voltage,current,temperature,energy}` - Real-time metrics

## Common Tasks

**Adding a new config type:**
1. Add struct in `include/device_state.h`
2. Implement request/update/set/get functions in `src/device_state.c`
3. Add file handling in `src/fuse_ops.c`
4. Request initial config in `main.c` on WebSocket open

**Adding a new proc file:**
1. Add field to appropriate status struct in `device_state.h`
2. Update parsing in `device_state.c`
3. Add path routing in `fuse_ops.c` read/write handlers

## Git Workflow

### Branching Strategy
- **main**: Production-ready code, only receives merges from dev
- **dev**: Development branch, created from main, where integration happens
- **feature branches**: Created from dev for each new feature or change

### Branch Naming
- Feature branches: `feature/<short-description>` (e.g., `feature/add-dimmer-support`)
- Bug fixes: `fix/<short-description>` (e.g., `fix/mac-validation`)

### Commit Workflow (Step by Step)

1. **Checkout dev branch:**
   ```bash
   git checkout dev
   ```

2. **Create feature branch from dev:**
   ```bash
   git checkout -b feature/<short-description>
   ```

3. **Stage and commit changes with descriptive message:**
   ```bash
   git add <file>
   git commit -m "$(cat <<'EOF'
   Short summary of changes

   - Detailed bullet point 1
   - Detailed bullet point 2
   - Detailed bullet point 3

   Co-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>
   EOF
   )"
   ```

4. **Test the feature before merging:**
   - For software-only changes: Run build and verify functionality
   - For hardware-dependent changes: **ASK the user to test manually**
   - Never merge untested code into dev

5. **ASK before merging to dev:**
   - Always ask the user for approval before merging feature into dev
   - Example: "Feature is ready and committed. May I merge to dev and main?"

6. **Merge feature branch to dev (with --no-ff to preserve branch history):**
   ```bash
   git checkout dev
   git merge feature/<short-description> --no-ff -m "Merge feature/<short-description> into dev"
   ```

7. **Merge dev to main (with --no-ff to preserve branch history):**
   ```bash
   git checkout main
   git merge dev --no-ff -m "Merge dev into main"
   ```

8. **Push both branches and clean up:**
   ```bash
   git push origin main
   git push origin dev
   git branch -d feature/<short-description>
   ```

### Important: Always Use --no-ff

Always use `--no-ff` (no fast-forward) when merging to create merge commits. This preserves the branch topology and makes the history visible in GitLens:

```
*   Merge dev into main
|\
| *   Merge feature/xyz into dev
| |\
| | * Actual commit message
| |/
```

### Commit Message Format

```
Short summary (imperative mood, max 50 chars)

- Bullet point describing change 1
- Bullet point describing change 2
- Bullet point describing change 3

Co-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>
```