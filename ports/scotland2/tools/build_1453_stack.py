#!/usr/bin/env python3
"""Build the explicit source port in dependency order; never package or install."""
import argparse,json,subprocess,sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[3]
ORDER=['bshook','customtypes','lapiz','bsml','metacore','metacore-bs','web-utils','beatsaverplusplus','songcore',
       'song-details','playlistcore','songdownloader','unicode','datakeeper','bettersongsearch','airbuds-search']
def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--workspace',type=Path,required=True)
    p.add_argument('--legacy-dependencies',type=Path,required=True);p.add_argument('--jobs',type=int,default=2)
    a=p.parse_args();work=a.workspace.resolve();legacy=a.legacy_dependencies.resolve()
    logs=work/'build/stack-logs';logs.mkdir(parents=True,exist_ok=True)
    for name in ORDER:
        print('Building',name,flush=True)
        command=[sys.executable,str(ROOT/'ports/scotland2/tools/build_1453_module.py'),name,
                 '--workspace',str(work),'--legacy-dependencies',str(legacy),'--jobs',str(a.jobs)]
        if name=='bshook':command.append('--sdk-tests')
        with (logs/(name+'.log')).open('w') as log:
            result=subprocess.run(command,cwd=ROOT,stdout=log,stderr=subprocess.STDOUT)
        if result.returncode:raise RuntimeError('Build failed: '+name+'; inspect '+str(logs/(name+'.log')))
    (logs/'completion.json').write_text(json.dumps({'compiled':ORDER,'scope':'Original components compiled in explicit dependency and source link order; runtime checks are separate'},indent=2)+'\n')
    print('Original component stack compiled; no game/runtime success is implied',flush=True)
if __name__=='__main__':main()
