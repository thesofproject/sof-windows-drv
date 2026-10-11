# Upstream Linux Sound Open Firmware (SOF) Tracking & Versioning

This document outlines how files shared with upstream Linux Sound Open Firmware are tracked, versioned, compared, and updated within the SOF Windows Driver repository.

---

## 1. Upstream Metadata & Provenance

| Property | Value |
| :--- | :--- |
| **Primary Repository** | [`https://github.com/thesofproject/linux`](https://github.com/thesofproject/linux) |
| **Default Branch** | `topic/sof-dev` |
| **Local Mirror (moth)** | `//192.168.122.1/work/linux` |
| **Tracked Commit** | [`b4bc873561db55f8ed1bb4113797d768424f300d`](https://github.com/thesofproject/linux/commit/b4bc873561db55f8ed1bb4113797d768424f300d) |
| **Commit Subject** | `ASoC: SOF: ipc4-topology: Fix shift-out-of-bounds for aggregated ALH capture` |
| **Commit Author** | Peter Ujfalusi `<peter.ujfalusi@linux.intel.com>` |
| **Tracked Files** | 48 files (`src/core/`, `include/intel/`, `include/sof/`) |
| **Machine Manifest** | [`UPSTREAM_LINUX.json`](./UPSTREAM_LINUX.json) |

---

## 2. Directory Structure & Separation Boundaries

The repository maintains strict architectural separation between Linux-shared code, the Windows OSAL (Operating System Abstraction Layer), and native Windows driver logic:

```
sof-windows-driver/
├── include/
│   ├── compat/          <-- [WINDOWS OSAL] Header shims (DO NOT OVERWRITE)
│   ├── intel/           <-- [UPSTREAM LINUX] Intel DSP registers & platform headers
│   └── sof/
│       ├── sof_osal.h   <-- [WINDOWS OSAL] KMDF bridge & primitives (DO NOT OVERWRITE)
│       ├── ipc4/        <-- [UPSTREAM LINUX] IPC4 message headers
│       └── uapi/        <-- [UPSTREAM LINUX] User API & ABI headers
├── src/
│   ├── core/            <-- [UPSTREAM LINUX] Core SOF IPC4 & DSP loader code
│   │   ├── intel/       <-- [UPSTREAM LINUX] Intel architecture drivers (PTL, MTL, TGL)
│   │   └── sof_core_init.c <-- [WINDOWS GLUE] ABI validation entry point
│   └── driver/          <-- [WINDOWS ONLY] Native KMDF/WaveRT/UAOL/PortCls driver
└── scripts/
    ├── sync_upstream_linux.py   <-- Synchronization & version management CLI
    └── verify_upstream_integrity.py <-- Pre-commit / CI integrity verification
```

> [!IMPORTANT]
> **Strict Linux Code Preservation Boundary**:
> 1. Files in `src/core/` and `include/` that are mirrored from Linux must **never** be manually modified with Windows-specific APIs or idioms.
> 2. All platform adaptations (types, mutexes, memory allocation, delays) are implemented via `include/sof/sof_osal.h` and `include/compat/`.
> 3. Files in `src/driver/` are 100% native Windows kernel code and are never touched by upstream syncs.

---

## 3. CLI Management Tool: `scripts/sync_upstream_linux.py`

The repository provides [`scripts/sync_upstream_linux.py`](./scripts/sync_upstream_linux.py) to manage upstream tracking.

### Commands

#### Check Current Status
Verify local file integrity and compare against upstream HEAD:
```powershell
python scripts/sync_upstream_linux.py status
```

#### Check Incoming Upstream Commits
View commits that have touched the tracked SOF files on GitHub:
```powershell
python scripts/sync_upstream_linux.py check-updates
```

#### Diff Against Upstream
Generate a unified diff between local files and an upstream branch or commit:
```powershell
# Diff against default branch (topic/sof-dev)
python scripts/sync_upstream_linux.py diff

# Diff a specific file
python scripts/sync_upstream_linux.py diff --file src/core/ipc4.c

# Diff against a specific commit or tag
python scripts/sync_upstream_linux.py diff --ref 458a4242a9
```

#### Update Tracked Files
Fetch updated files from upstream, update `UPSTREAM_LINUX.json`, and run MSBuild to verify OSAL compatibility:
```powershell
# Dry run preview (no files changed)
python scripts/sync_upstream_linux.py update --dry-run

# Update to latest commit on default branch (topic/sof-dev)
python scripts/sync_upstream_linux.py update

# Update to a specific commit or tag
python scripts/sync_upstream_linux.py update --ref <commit-sha>

# Update from local mirror worktree
python scripts/sync_upstream_linux.py update --source //192.168.122.1/work/linux
```

#### Verify Integrity (CI / Pre-commit)
Ensure no Linux-shared files were modified without a manifest update:
```powershell
python scripts/sync_upstream_linux.py verify
```

---

## 4. Tracked File Mappings (48 Files)

### `src/core/` — Core Protocol & Pipeline
| Upstream Linux Path | Local Windows Path | Category |
| :--- | :--- | :--- |
| `sound/soc/sof/fw-file-profile.c` | `src/core/fw-file-profile.c` | Core C |
| `sound/soc/sof/ipc4-control.c` | `src/core/ipc4-control.c` | Core C |
| `sound/soc/sof/ipc4-fw-reg.h` | `src/core/ipc4-fw-reg.h` | Core Header |
| `sound/soc/sof/ipc4-loader.c` | `src/core/ipc4-loader.c` | Core C |
| `sound/soc/sof/ipc4-priv.h` | `src/core/ipc4-priv.h` | Core Header |
| `sound/soc/sof/ipc4-topology.c` | `src/core/ipc4-topology.c` | Core C |
| `sound/soc/sof/ipc4-topology.h` | `src/core/ipc4-topology.h` | Core Header |
| `sound/soc/sof/ipc4.c` | `src/core/ipc4.c` | Core C |
| `sound/soc/sof/loader.c` | `src/core/loader.c` | Core C |
| `sound/soc/sof/ops.h` | `src/core/ops.h` | Core Header |
| `sound/soc/sof/sof-audio.h` | `src/core/sof-audio.h` | Core Header |
| `sound/soc/sof/sof-priv.h` | `src/core/sof-priv.h` | Core Header |

### `src/core/intel/` — Intel Microarchitectures
| Upstream Linux Path | Local Windows Path | Category |
| :--- | :--- | :--- |
| `sound/soc/sof/intel/hda-dsp.c` | `src/core/intel/hda-dsp.c` | Intel Core C |
| `sound/soc/sof/intel/mtl.c` | `src/core/intel/mtl.c` | Intel Core C |
| `sound/soc/sof/intel/ptl.c` | `src/core/intel/ptl.c` | Intel Core C |
| `sound/soc/sof/intel/tgl.c` | `src/core/intel/tgl.c` | Intel Core C |

### `include/intel/` — Intel Hardware Registers
| Upstream Linux Path | Local Windows Path | Category |
| :--- | :--- | :--- |
| `sound/soc/sof/intel/atom.h` | `include/intel/atom.h` | Intel Header |
| `sound/soc/sof/intel/ext_manifest.h` | `include/intel/ext_manifest.h` | Intel Header |
| `sound/soc/sof/intel/hda-ipc.h` | `include/intel/hda-ipc.h` | Intel Header |
| `sound/soc/sof/intel/hda.h` | `include/intel/hda.h` | Intel Header |
| `sound/soc/sof/intel/lnl.h` | `include/intel/lnl.h` | Intel Header |
| `sound/soc/sof/intel/mtl.h` | `include/intel/mtl.h` | Intel Header |
| `sound/soc/sof/intel/nvl.h` | `include/intel/nvl.h` | Intel Header |
| `sound/soc/sof/intel/ptl.h` | `include/intel/ptl.h` | Intel Header |
| `sound/soc/sof/intel/shim.h` | `include/intel/shim.h` | Intel Header |
| `sound/soc/sof/intel/telemetry.h` | `include/intel/telemetry.h` | Intel Header |

### `include/sof/` — SOF Core Interfaces
| Upstream Linux Path | Local Windows Path | Category |
| :--- | :--- | :--- |
| `include/sound/sof/channel_map.h` | `include/sof/channel_map.h` | SOF Header |
| `include/sound/sof/control.h` | `include/sof/control.h` | SOF Header |
| `include/sound/sof/dai-amd.h` | `include/sof/dai-amd.h` | SOF Header |
| `include/sound/sof/dai-imx.h` | `include/sof/dai-imx.h` | SOF Header |
| `include/sound/sof/dai-intel.h` | `include/sof/dai-intel.h` | SOF Header |
| `include/sound/sof/dai-mediatek.h` | `include/sof/dai-mediatek.h` | SOF Header |
| `include/sound/sof/dai.h` | `include/sof/dai.h` | SOF Header |
| `include/sound/sof/debug.h` | `include/sof/debug.h` | SOF Header |
| `include/sound/sof/ext_manifest.h` | `include/sof/ext_manifest.h` | SOF Header |
| `include/sound/sof/ext_manifest4.h` | `include/sof/ext_manifest4.h` | SOF Header |
| `include/sound/sof/header.h` | `include/sof/header.h` | SOF Header |
| `include/sound/sof/info.h` | `include/sof/info.h` | SOF Header |
| `include/sound/sof/pm.h` | `include/sof/pm.h` | SOF Header |
| `include/sound/sof/stream.h` | `include/sof/stream.h` | SOF Header |
| `include/sound/sof/topology.h` | `include/sof/topology.h` | SOF Header |
| `include/sound/sof/trace.h` | `include/sof/trace.h` | SOF Header |
| `include/sound/sof/xtensa.h` | `include/sof/xtensa.h` | SOF Header |

### `include/sof/ipc4/` & `include/sof/uapi/`
| Upstream Linux Path | Local Windows Path | Category |
| :--- | :--- | :--- |
| `include/sound/sof/ipc4/header.h` | `include/sof/ipc4/header.h` | IPC4 Header |
| `include/uapi/sound/sof/abi.h` | `include/sof/uapi/abi.h` | UAPI Header |
| `include/uapi/sound/sof/fw.h` | `include/sof/uapi/fw.h` | UAPI Header |
| `include/uapi/sound/sof/header.h` | `include/sof/uapi/header.h` | UAPI Header |
| `include/uapi/sound/sof/tokens.h` | `include/sof/uapi/tokens.h` | UAPI Header |

---

## 5. Git Commit & Tagging Standard

When synchronizing files from upstream:
1. **Commit Message Format**:
   ```
   core: Sync upstream Linux SOF to thesofproject/linux@<short-sha> (<subject>)
   
   Synchronized 48 shared files from https://github.com/thesofproject/linux:
   Upstream commit: <full-sha>
   Branch: topic/sof-dev
   Author: <author>
   Date: <date>
   
   Verified with MSBuild Release|x64: 0 errors, 0 warnings.
   ```
2. **Git Tag**:
   ```powershell
   git tag -a linux-sync/<short-sha> -m "Synced upstream Linux SOF to <short-sha>"
   ```
