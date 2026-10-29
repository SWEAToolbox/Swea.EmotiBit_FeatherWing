# tools

`emotibit.py` backs up, flashes, restores and monitors EmotiBit boards. Set it up with `setup.ps1` (Windows) or `setup.sh` (Linux).

Full guide, requirements for Windows and Linux, and troubleshooting: SWEA.Documentation, `docfx/articles/emotibit/flashning.md`.

```
EB check                       # requirements on this computer
EB ports                       # serial ports (CP210x is the EmotiBit)
EB backup  --port COM7         # full 4 MB flash backup (do this first, once per board)
EB flash   --port COM7 --monitor 15
EB restore --port COM7         # write the backup back
EB monitor --port COM7 --reset # serial log, Wi-Fi passwords hidden
EB scan                        # BLE advertisement
```

`EB` = `.venv-tools\Scripts\python.exe tools\emotibit.py` (Windows) or `.venv-tools/bin/python tools/emotibit.py` (Linux).
`flash` refuses to run for a board that has no backup.
