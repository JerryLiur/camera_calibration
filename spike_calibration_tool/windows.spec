# -*- mode: python ; coding: utf-8 -*-
from pathlib import Path
from PyInstaller.utils.hooks import collect_data_files, copy_metadata

root = Path(SPECPATH).resolve().parent
datas = [(str(root / "spike_calibration_tool" / "web"), "spike_calibration_tool/web")]
datas += collect_data_files("webview")
for package in ("pywebview", "numpy"):
    datas += copy_metadata(package)

a = Analysis([str(root / "spike_calibration_tool" / "launch_desktop.py")],
    pathex=[str(root)], binaries=[], datas=datas,
    hiddenimports=["webview.platforms.winforms", "webview.platforms.edgechromium"],
    hookspath=[], hooksconfig={}, runtime_hooks=[],
    excludes=["torch", "torchvision", "tensorflow", "ultralytics", "matplotlib", "pandas", "sklearn",
              "PyQt5", "PyQt6", "PySide2", "PySide6", "cefpython3", "gi", "gtk", "tkinter",
              "IPython", "jupyter", "pytest"], noarchive=False, optimize=1)
pyz = PYZ(a.pure)
exe = EXE(pyz, a.scripts, [], exclude_binaries=True, name="Spike标定工具",
          debug=False, bootloader_ignore_signals=False, strip=False, upx=False,
          console=False, disable_windowed_traceback=False)
coll = COLLECT(exe, a.binaries, a.datas, strip=False, upx=False, name="Spike标定工具")
