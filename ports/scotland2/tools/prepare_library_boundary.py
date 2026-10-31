#!/usr/bin/env python3
"""Prepare one exact exported library entry before Apple signing."""
import argparse,pathlib,subprocess,sys,tempfile
ROOT=pathlib.Path(__file__).resolve().parents[3]
def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--elf',type=pathlib.Path,required=True);p.add_argument('--output',type=pathlib.Path,required=True);p.add_argument('--symbol',required=True);p.add_argument('--platform',choices=['macos','visionos'],required=True);a=p.parse_args()
 a.output.parent.mkdir(parents=True,exist_ok=True)
 with tempfile.TemporaryDirectory(prefix='sc2-boundary-',dir=a.output.parent) as tmp:
  tmp=pathlib.Path(tmp);translation=tmp/'unprepared.dylib';manifest=tmp/'manifest.json'
  name=a.output.stem
  install='@rpath/'+(name+'.framework/'+name if a.platform=='visionos' else name+'.dylib')
  commands=[[ROOT/'build/klepton-ld',a.elf,'--platform',a.platform,'--install-name',install,'-o',translation,'--quiet'],[sys.executable,ROOT/'ports/scotland2/tools/hook_manifest.py','--elf',a.elf,'--translation',translation,'--output',manifest,a.symbol],[sys.executable,ROOT/'ports/scotland2/tools/prepare_hooks.py','--elf',a.elf,'--input',translation,'--manifest',manifest,'--output',a.output]]
  for cmd in commands:subprocess.run([str(x) for x in cmd],cwd=ROOT,check=True)
if __name__=='__main__':main()
