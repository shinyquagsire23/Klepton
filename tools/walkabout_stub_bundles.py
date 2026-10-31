import struct, sys, os, hashlib
def lz4(src, usize):
    out=bytearray(); i=0
    while i < len(src):
        t=src[i]; i+=1
        l=t>>4
        if l==15:
            while True:
                b=src[i]; i+=1; l+=b
                if b!=255: break
        out+=src[i:i+l]; i+=l
        if i>=len(src): break
        off=src[i]|(src[i+1]<<8); i+=2
        m=t&15
        if m==15:
            while True:
                b=src[i]; i+=1; m+=b
                if b!=255: break
        m+=4
        for _ in range(m): out.append(out[-off])
    assert len(out)==usize, (len(out), usize)
    return bytes(out)
def cstr(d,p):
    e=d.index(b'\0',p); return d[p:e], e+1
def stub(donor, name, outpath):
    d=open(donor,'rb').read()
    p=0; sig,p=cstr(d,p); ver=struct.unpack('>I',d[p:p+4])[0]; p+=4
    uv,p=cstr(d,p); rev,p=cstr(d,p)
    size,csz,usz,flags=struct.unpack('>qIII',d[p:p+20]); hdr_fields=p; p+=20
    if ver>=7: p=(p+15)&~15
    bi=d[p:p+csz]; p+=csz
    comp=flags&0x3f
    info=lz4(bi,usz) if comp in (2,3) else bi
    if flags&0x200: p=(p+15)&~15
    data=d[p:]
    # blocks info: 16B hash, u32 nblocks, nblocks*(u32,u32,u16), u32 nnodes, nodes(q,q,I,cstr)
    q=16; nb=struct.unpack('>I',info[q:q+4])[0]; q+=4+nb*10
    nn=struct.unpack('>I',info[q:q+4])[0]; q+=4
    info=bytearray(info)
    for k in range(nn):
        q+=20
        path,e=cstr(bytes(info),q)
        new=('CAB-'+hashlib.md5((name+str(k)).encode()).hexdigest()).encode()
        tail=path[len(new):] if path.startswith(b'CAB-') and len(path)>=len(new) else b''
        assert path.startswith(b'CAB-') and len(path)>=36, path
        info[q:q+36]=new
        print('  node', path.decode(), '->', bytes(info[q:e-1]).decode())
        q=e
    nflags=flags&~0x3f
    head=d[:hdr_fields]
    out=bytearray(head)+struct.pack('>qIII',0,len(info),len(info),nflags)
    if ver>=7: out+=b'\0'*((-len(out))%16)
    out+=info
    if nflags&0x200: out+=b'\0'*((-len(out))%16)
    out+=data
    struct.pack_into('>q',out,hdr_fields,len(out))
    open(outpath,'wb').write(out)
donor=sys.argv[1]; outdir=sys.argv[2]
os.makedirs(outdir,exist_ok=True)
for n in sys.argv[3:]:
    fn=n+'common_assets_.bundle'
    stub(donor, fn, os.path.join(outdir,fn))
    print('wrote', fn)
