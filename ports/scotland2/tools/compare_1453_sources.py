#!/usr/bin/env python3
"""Compare independently reconstructed source inputs to the actual port workspace."""
import argparse,hashlib,json
from pathlib import Path
def files(tree):
    return {str(p.relative_to(tree)):p for p in tree.rglob('*')
            if p.is_file() and not p.is_symlink() and
            not any(x.is_symlink() for x in p.parents if x!=tree) and
            p.suffix not in {'.orig','.rej','.merge-candidate'}}
def compare(actual,proof,output):
    rows=[];bad=[];excluded=[]
    for tree in sorted((proof/'work-sources').iterdir()):
        if not tree.is_dir():continue
        source=actual/'work-sources'/tree.name
        if not source.exists():source=actual/'sources'/tree.name
        a,b=files(tree),files(source)
        for name in sorted(a):
            if name not in b or hashlib.sha256(a[name].read_bytes()).digest()!=hashlib.sha256(b[name].read_bytes()).digest():
                bad.append({'module':tree.name,'file':name})
        for name in sorted(b.keys()-a.keys()):
            # The pinned Airbuds selected-blob archive deliberately omits its
            # bundled build tooling/screenshots. No compiled source/header or
            # asset may disappear behind this exception.
            allowed=tree.name=='airbuds-search' and (name.startswith(('.github/','cmake/','docs/','hooks/','scripts/','tools/')) or
                    name in {'.clang-format','.gitignore','cover.jpg','extern.cmake','qpm.shared.json'})
            (excluded if allowed else bad).append({'module':tree.name,'file':name})
        rows.append({'module':tree.name,'filesCompared':len(a)})
    report={'scope':'Independent source archive/patch reconstruction compared to actual build source inputs; not a runtime test',
            'trees':rows,'excludedUpstreamBuildTooling':excluded,'mismatches':bad}
    output.write_text(json.dumps(report,indent=2)+'\n')
    if bad:raise ValueError('Source reconstruction mismatches: '+str(bad[:10]))
    print('SOURCE_RECONSTRUCTION_OK',len(rows),'trees',sum(r['filesCompared'] for r in rows),'files; zero source/header/asset mismatches')
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--actual',type=Path,required=True)
    p.add_argument('--proof',type=Path,required=True);p.add_argument('--output',type=Path,required=True)
    a=p.parse_args();compare(a.actual.resolve(),a.proof.resolve(),a.output.resolve())
