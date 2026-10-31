"""Normalize private build paths in release artifacts, without changing selection."""
import os
from pathlib import Path
def reproducible_flags(root,ndk=None):
 if os.environ.get('SC2_BUILD_MODE')!='release':return []
 flags=['-ffile-prefix-map='+str(root)+'=.','-fmacro-prefix-map='+str(root)+'=.','-fdebug-compilation-dir=.']
 if ndk:
  for path in dict.fromkeys([str(ndk),str(Path(ndk).resolve())]):flags.append('-ffile-prefix-map='+path+'=/ndk')
 return flags
