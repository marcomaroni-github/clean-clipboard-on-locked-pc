# CleanClipboardOnLock

Utility ultra-leggera per Windows scritta in **C puro (API Win32 native, senza C Runtime / CRT)** che svuota automaticamente sia gli **Appunti correnti (`Ctrl+V`)** sia la **Cronologia Appunti di Windows 10/11 (`Win+V`)** non appena la sessione viene bloccata (`Win+L`, timeout schermo, blocco sessione).

## Caratteristiche principali

- **Consumo RAM minimo (~0.3 – 0.5 MB):** compilata senza CRT (`/NODEFAULTLIB`), senza framework (.NET/Electron) e senza finestre grafiche (`HWND_MESSAGE`). La libreria WinRT (`combase.dll`) per pulire la cronologia `Win+V` viene caricata dinamicamente solo nell'istante del blocco e subito scaricata, seguito da `SetProcessWorkingSetSize` per rilasciare immediatamente le pagine al sistema operativo.
- **0.00% CPU in background:** nessun timer o polling; il processo dorme nel kernel (`GetMessageW`) finché Windows non invia la notifica `WM_WTSSESSION_CHANGE` (`WTS_SESSION_LOCK`).
- **Completamente invisibile:** nessuna finestra, nessuna icona nella barra delle applicazioni o System Tray.
- **Doppia modalità nello stesso eseguibile (~6 KB):**
  1. **Demone invisibile (consigliato, nessun privilegio Admin richiesto):** resta in ascolto passivo e avvia automaticamente al login utente (`HKCU\Software\Microsoft\Windows\CurrentVersion\Run`).
  2. **One-shot (`--once`, 0 byte RAM in idle):** pulisce tutto in pochi millisecondi ed esce subito, ideale se si preferisce agganciarlo all'*Utilità di pianificazione* (Task Scheduler) di Windows sull'evento *Al blocco della sessione*.

## Compilazione

Richiede Visual Studio con il workload *Sviluppo di applicazioni desktop con C++* (o *MSVC Build Tools*):

```powershell
.\build.ps1
```

L'eseguibile viene generato in `dist\CleanClipboardOnLock.exe`.

## Utilizzo

### Modalità 1 (Consigliata): Demone invisibile con avvio automatico

```powershell
# Registra l'avvio automatico per l'utente corrente e avvia subito il processo in background
.\dist\CleanClipboardOnLock.exe --install

# Per rimuovere l'avvio automatico e terminare il processo in background
.\dist\CleanClipboardOnLock.exe --uninstall
```

Altri comandi utili:

| Parametro | Descrizione |
|---|---|
| *(nessuno)* | Avvia il demone invisibile in background (singola istanza garantita da Mutex) |
| `--install` | Configura l'avvio automatico in `HKCU\...\Run` e avvia subito il demone |
| `--uninstall` | Termina l'istanza attiva e rimuove la chiave di avvio automatico |
| `--stop` | Termina silenziosamente l'istanza attiva in background |
| `--once` | Svuota immediatamente Clipboard + Cronologia (`Win+V`) e termina il processo |
| `--help` | Mostra la finestra riepilogativa dei comandi |

### Modalità 2 (Alternativa): Zero processi in background tramite Utilità di Pianificazione

Se preferisci **0 processi attivi** in background (richiede PowerShell come **Amministratore** per registrare il trigger di sessione di Windows):

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
