# WSLg: Weston and /mnt/shared_memory

Reviewed 2026-10-02.

## Summary

The `/mnt/shared_memory` mount isn't configurable. WSLg mounts it itself at a
fixed path, and on this machine it's failing, most likely from the WSL bug
fixed after the version installed here (2.7.3).

## How it's set up

From `WSLGd/main.cpp` in microsoft/wslg:

- WSLGd runs inside WSLg's own hidden Linux distro, not inside Ubuntu. That's
  why `/mnt/shared_memory` doesn't exist in the Ubuntu shell; that much is
  normal.
- The mount point is hardcoded as `/mnt/shared_memory`. It's a virtiofs share
  named `wslg`, mounted with the `dax` option:
  `mount("wslg", "/mnt/shared_memory", "virtiofs", 0, "dax")`.
  WSLGd only tries the mount when Windows sets
  `WSL2_SHARED_MEMORY_OB_DIRECTORY`.
- If the mount fails, WSLGd logs an error, unsets
  `WSL2_SHARED_MEMORY_MOUNT_POINT`, and carries on without shared memory.
- Weston's RDP backend (`rdp_allocate_shared_memory` in weston-mirror
  `libweston/backend-rdp/rdputil.c`) creates one file per buffer in that mount
  (`/mnt/shared_memory/{GUID}`, opened `O_CREAT|O_RDWR|O_EXCL`, sized with
  `fallocate`), then maps it with `mmap(MAP_SHARED)`. Without shared memory,
  WSLg copies the pixels to Windows instead ("COPY MODE", shown as
  `[WARN:COPY MODE]` in window titles).

## Configuration

- The README only documents `guiApplications=false` under `[wsl2]` in
  `%USERPROFILE%\.wslconfig`.
- The code also reads `%USERPROFILE%\.wslgconfig` (falling back to
  `C:\ProgramData\Microsoft\WSL\.wslgconfig`). The only section it reads is
  `[system-distro-env]`, and each key there is set as an environment variable
  in WSLg's distro.
- Nothing there changes the mount path, the filesystem type or the size.
- This machine has no `.wslgconfig`; `.wslconfig` only sets
  `MaxCrashDumpCount=-1` and `networkingMode=mirrored`.

## What this machine shows

- Versions: WSL 2.7.3.0, WSLg 1.0.73, kernel 6.6.114.1-1, Windows
  10.0.26200.9550.
- `/mnt/wslg/weston.log` (Weston started 2026-09-26 10:36) has this once at
  startup:

  ```
  rdp_allocate_shared_memory: Failed to open "/mnt/shared_memory/{5dcc78af-b990-473c-ad18-a206a53862cc}" with error: Input/output error
  ```

- `WSL2_SHARED_MEMORY_MOUNT_POINT=/mnt/shared_memory` is still set in Weston's
  environment. Since WSLGd unsets it when the mount fails, the mount
  apparently succeeded and creating a file in it is what failed. That points
  at the Windows side of the share, not a missing mount.
- There's no "COPY MODE" text in weston.log, and window titles weren't
  checked, so it's unknown whether apps are actually running in copy mode.

## Known issues

- [microsoft/WSL#40618](https://github.com/microsoft/WSL/issues/40618): WSL
  2.7.3 with the same kernel as this machine, showing the same error,
  invisible windows and `[WARN:COPY MODE]` in window titles. It was closed by
  [PR #41499](https://github.com/microsoft/WSL/pull/41499), which updates WSL
  DeviceHost to 1.2.62 with a "virtiofs section fix". That update first
  shipped in pre-release 2.9.10 (2026-09-04).
- [wslg#1320](https://github.com/microsoft/wslg/issues/1320),
  [#827](https://github.com/microsoft/wslg/issues/827) and
  [#179](https://github.com/microsoft/wslg/issues/179) report the same error
  on older versions. They're still open, with no root cause from the
  maintainers.
- [wslg#1483](https://github.com/microsoft/wslg/issues/1483): the virtiofs
  request hangs instead of failing, and Weston can't be killed. Only
  `wsl --shutdown` recovers.

## Recommendations

1. Update WSL. 3.0.1 (2026-09-29) is the latest stable release and is newer
   than the fix, though it wasn't confirmed to include DeviceHost 1.2.62.
   Pre-release 2.9.10 or later definitely has it:
   `wsl --update --pre-release`.
2. If GUI windows freeze or go blank before then, run `wsl --shutdown` from
   Windows to restart WSLg's distro along with everything else.
3. Skip the workaround posted in #40618
   (`sudo mount -t tmpfs tmpfs /mnt/shared_memory`). Run in Ubuntu, it mounts
   in a different distro from the one Weston uses, so it can't reach the
   failing mount. The fix it seemed to give was more likely a coincidental
   WSLg restart.

## Sources

- [microsoft/wslg README](https://github.com/microsoft/wslg)
- [WSLGd/main.cpp](https://raw.githubusercontent.com/microsoft/wslg/main/WSLGd/main.cpp)
- [weston-mirror rdputil.c](https://raw.githubusercontent.com/microsoft/weston-mirror/working/libweston/backend-rdp/rdputil.c)
- [WSL#40618](https://github.com/microsoft/WSL/issues/40618),
  [WSL PR #41499](https://github.com/microsoft/WSL/pull/41499),
  [WSL releases](https://github.com/microsoft/WSL/releases)
- [wslg#1320](https://github.com/microsoft/wslg/issues/1320),
  [wslg#1483](https://github.com/microsoft/wslg/issues/1483),
  [wslg#827](https://github.com/microsoft/wslg/issues/827),
  [wslg#179](https://github.com/microsoft/wslg/issues/179)
