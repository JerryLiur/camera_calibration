"""PyInstaller entry file; also supports self-test/serve arguments."""
from spike_calibration_tool.cli import main

if __name__ == "__main__":
    raise SystemExit(main())
