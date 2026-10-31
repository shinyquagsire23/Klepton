#!/usr/bin/env python3
"""Rebuild reconstructed inputs in a second workspace and compare actual ELF bytes."""
import argparse,json,shutil,subprocess,sys
from pathlib import Path
import bootstrap
from build_1453_stack import ORDER
ROOT=Path(__file__).resolve().parents[3]
def check(work,reference,legacy,jobs,prune):
    if work==reference or work.is_relative_to(reference/'build'):
        raise ValueError('Use a separate reconstructed workspace')
    receipt=json.loads((work/'source-reconstruction-receipt.json').read_text())
    manifest=ROOT/'ports/scotland2/port1453/inputs.json'
    manifest_hash=bootstrap.digest(manifest)
    if receipt['manifestSha256']!=manifest_hash:raise ValueError('Reconstruct the current pinned source manifest first')
    references={p.name:bootstrap.digest(p) for p in (reference/'build/libs').glob('*.so')}
    logs=work/'build/repro-logs';logs.mkdir(parents=True,exist_ok=True)
    rows=[];output=work/'build/reproducibility.json'
    def save(passed=False):
        output.write_text(json.dumps({'passed':passed,'manifestSha256':manifest_hash,'libraries':rows,
            'scope':'Actual second-workspace compilation/ELF byte comparison; no device/signing or gameplay claim'},indent=2)+'\n')
    def build(tool,extra,label):
        if bootstrap.digest(manifest)!=manifest_hash:save();raise ValueError('Source manifest changed during the independent rebuild')
        print('Rebuild',label,flush=True)
        with (logs/(label+'.log')).open('w') as log:
            result=subprocess.run([sys.executable,str(ROOT/'ports/scotland2/tools'/tool),*extra,
                '--workspace',str(work)],cwd=ROOT,stdout=log,stderr=subprocess.STDOUT)
        if result.returncode:save();raise RuntimeError('Independent compilation failed: '+label)
    def compare(name):
        rebuilt=work/'build/libs'/name;original=reference/'build/libs'/name
        actual=bootstrap.digest(rebuilt);expected=references[name]
        if bootstrap.digest(original)!=expected:save();raise ValueError('Reference output changed during the independent rebuild: '+name)
        same=actual==expected;rows.append({'library':name,'sha256':actual,'referenceSha256':expected,'identical':same});save()
        if not same:raise ValueError('Independent output differs: '+name)
        print('Identical',name,actual,flush=True)
    build('build_1453_support.py',['--legacy-dependencies',str(legacy)],'support')
    compare('libsl2_abi.so');compare('libc++_shared.so')
    build('build_1453_flamingo.py',[],'flamingo')
    compare('libflamingo.so');compare('libsc2_scene.so')
    from build_1453_module import MODULES
    for name in ORDER:
        build('build_1453_module.py',[name,'--legacy-dependencies',str(legacy),'--jobs',str(jobs)],name)
        compare(MODULES[name][2]+'.so')
        # These are only this proof's newly generated caches/debug ELFs. Keep
        # all reconstructed source inputs, shipping outputs, logs and receipts.
        if prune:
            shutil.rmtree(work/'build'/name/'objects')
            for path in (work/'build'/name).glob('*.debug.so'):path.unlink()
    for path in sorted((work/'build/libs').glob('*.so')):
        if path.name not in {r['library'] for r in rows}:compare(path.name)
    if bootstrap.digest(manifest)!=manifest_hash:raise ValueError('Source manifest changed during the independent rebuild')
    save(True)
    print('REPRODUCIBLE_1453_ELFS_OK',len(rows),'actual library outputs match across independent source workspaces',flush=True)
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--workspace',type=Path,required=True)
    p.add_argument('--reference',type=Path,required=True);p.add_argument('--legacy-dependencies',type=Path,required=True)
    p.add_argument('--jobs',type=int,default=2);p.add_argument('--prune-generated-caches',action='store_true')
    a=p.parse_args();check(a.workspace.resolve(),a.reference.resolve(),a.legacy_dependencies.resolve(),a.jobs,a.prune_generated_caches)
