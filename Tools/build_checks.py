"""Lightweight PlatformIO preflight, after compatibility-sketch generation."""
from pathlib import Path
import runpy

Import('env')
root = Path(env.subst('$PROJECT_DIR'))
validation = runpy.run_path(str(root / 'Tools' / 'validate.py'))
validation['check_source'](root)
