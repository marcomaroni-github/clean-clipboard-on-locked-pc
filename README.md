# CleanClipboardOnLock

An ultra-lightweight Windows utility written in **pure C (native Win32 API, no C Runtime / CRT)** that automatically clears both the **current clipboard (`Ctrl+V`)** and the **Windows 10/11 Clipboard History (`Win+V`)** as soon as the session is locked (`Win+L`, display timeout, session lock).

## Key Features

- **Minimal RAM usage (~0.3 – 0.5 MB):** compiled without CRT (`/NODEFAULTLIB`), without frameworks (.NET/Electron), and without graphical windows (`HWND_MESSAGE`). The WinRT library (`combase.dll`) required to clear `Win+V` history is dynamically loaded only at the moment of lock and immediately unloaded, followed by `SetProcessWorkingSetSize` to release pages back to the OS immediately.
- **0.00% CPU in the background:** no timers or polling; the process sleeps in the kernel (`GetMessageW`) until Windows dispatches the `WM_WTSSESSION_CHANGE` (`WTS_SESSION_LOCK`) notification.
- **Completely invisible:** no window, no taskbar entry, and no System Tray icon.
- **Dual mode in the same executable (~6 KB):**
  1. **Invisible daemon (recommended, no Admin privileges required):** stays in passive listening mode and starts automatically at user login (`HKCU\Software\Microsoft\Windows\CurrentVersion\Run`).
  2. **One-shot (`--once`, 0 bytes idle RAM):** clears everything in a few milliseconds and exits immediately, ideal for pairing with Windows *Task Scheduler* on the *On workstation lock* event.

## Building

Requires Visual Studio with the *Desktop development with C++* workload (or *MSVC Build Tools*):

```powershell
.\build.ps1
```

The executable will be generated at `dist\CleanClipboardOnLock.exe`.

## Usage

### Mode 1 (Recommended): Invisible daemon with autostart

```powershell
# Register autostart for the current user and start the background process immediately
.\dist\CleanClipboardOnLock.exe --install

# Remove autostart and terminate the background process
.\dist\CleanClipboardOnLock.exe --uninstall
```

Other available options:

| Parameter | Description |
|---|---|
| *(none)* | Starts the invisible daemon in the background (single instance enforced via Mutex) |
| `--install` | Configures autostart in `HKCU\...\Run` and starts the daemon immediately |
| `--uninstall` | Terminates the active instance and removes the autostart registry key |
| `--stop` | Silently terminates the background instance |
| `--once` | Immediately clears Clipboard + History (`Win+V`) and exits |
| `--help` | Displays a summary dialog with available commands |

### Mode 2 (Alternative): Zero background processes via Task Scheduler

If you prefer **0 active processes** in the background (requires PowerShell run as **Administrator** to register the Windows session state change trigger):

```powershell
$exe = (Resolve-Path .\dist\CleanClipboardOnLock.exe).Path
$stateChangeTrigger = Get-CimClass -Namespace Root\Microsoft\Windows\TaskScheduler -ClassName MSFT_TaskSessionStateChangeTrigger
$trigger = New-CimInstance -CimClass $stateChangeTrigger -ClientOnly -Property @{
    StateChange = 7 # 7 = TASK_SESSION_LOCK
    UserId      = "$env:USERDOMAIN\$env:USERNAME"
}
$action   = New-ScheduledTaskAction -Execute $exe -Argument "--once"
$settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -ExecutionTimeLimit (New-TimeSpan -Minutes 1)

Register-ScheduledTask -TaskName "CleanClipboardOnLock" -Trigger $trigger -Action $action -Settings $settings -Force
```
