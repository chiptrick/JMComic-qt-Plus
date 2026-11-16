import sys, struct
sys.path.insert(0,'cpp/tools')
from onnx_ops_inventory import fields, read_varint

def read_tensor(v):
    dims=[]; name=''; dtype=0; raw=b''
    for f,w,x in fields(v):
        if f==1:
            if w==0: dims.append(x)
            else:
                j=0
                while j<len(x): d,j=read_varint(x,j); dims.append(d)
        elif f==2 and w==0: dtype=x
        elif f==8 and w==2: name=x.decode()
        elif f==9 and w==2: raw=x
    return dims,dtype,raw,name

def node_info(n):
    op=''; ins=[]; outs=[]; attrs={}
    for f,w,x in fields(n):
        if f==1 and w==2: ins.append(x.decode())
        elif f==2 and w==2: outs.append(x.decode())
        elif f==4 and w==2: op=x.decode()
        elif f==5 and w==2:
            nm=''; ints=[]; i1=None; s=''
            for a,b,c in fields(x):
                if a==1 and b==2: nm=c.decode()
                elif a==3 and b==0: i1=c
                elif a==4 and b==2: s=c.decode()
                elif a==8:
                    if b==0: ints.append(c)
                    else:
                        j=0
                        while j<len(c): d,j=read_varint(c,j); ints.append(d)
            attrs[nm]= s if s else (ints if ints else i1)
    return op,ins,outs,attrs

def vals_of(raw,dt):
    if dt==1: return list(struct.unpack('<%df'%(len(raw)//4), raw[:len(raw)//4*4]))
    if dt==7: return list(struct.unpack('<%dq'%(len(raw)//8), raw[:len(raw)//8*8]))
    if dt==6: return list(raw)
    if dt==11: return list(struct.unpack('<%dd'%(len(raw)//8), raw[:len(raw)//8*8]))
    return []

def load(path):
    data=open(path,'rb').read()
    graph=[v for f,w,v in fields(data) if f==7 and w==2][0]
    inits={}; nodes=[]; gin=[]; gout=[]
    for f,w,v in fields(graph):
        if f==1 and w==2: nodes.append(v)
        elif f==5 and w==2:
            d,dt,raw,nm=read_tensor(v); inits[nm]=(d,dt,raw)
        elif f==11 and w==2:
            for f2,w2,v2 in fields(v):
                if f2==1 and w2==2: gin.append(v2.decode())
        elif f==12 and w==2:
            for f2,w2,v2 in fields(v):
                if f2==1 and w2==2: gout.append(v2.decode())
    return inits,nodes,gin,gout

def infer(path,H=64,verbose=False):
    inits,nodes,gin,gout=load(path)
    v={}
    for nm,(d,dt,raw) in inits.items(): v[nm]=vals_of(raw,dt) if len(d)<=2 else None
    shp={}
    for nm,(d,dt,raw) in inits.items(): shp[nm]=list(d)
    shp[gin[0]]=[1,3,H,H]
    def g(n): return shp.get(n)
    for i,n in enumerate(nodes):
        op,ins,outs,at=node_info(n)
        if not ins:
            shp[outs[0]]=[1]
            continue
        a=g(ins[0])
        if a is None:
            if verbose: print('  %2d %-16s ??? %s'%(i,op,ins))
            continue
        o=None
        if op=='Conv':
            k=at['kernel_shape']; s=at.get('strides',[1,1]); p=at.get('pads',[0,0,0,0])
            oc=inits[ins[1]][0][0]
            o=[1,oc,(a[2]+p[0]+p[2]-k[0])//s[0]+1,(a[3]+p[1]+p[3]-k[1])//s[1]+1]
        elif op=='ConvTranspose':
            k=at['kernel_shape']; s=at.get('strides',[1,1]); p=at.get('pads',[0,0,0,0])
            opd=at.get('output_padding',[0,0])
            oc=inits[ins[1]][0][1]
            o=[1,oc,(a[2]-1)*s[0]-p[0]-p[2]+k[0]+opd[0],(a[3]-1)*s[1]-p[1]-p[3]+k[1]+opd[1]]
        elif op=='Pad':
            pv=v.get(ins[1]) or vals_of(inits[ins[1]][2],inits[ins[1]][1])
            if len(pv)==8:
                o=[1,a[1],a[2]+pv[2]+pv[6],a[3]+pv[3]+pv[7]]
            else: o=list(a)
        elif op in ('LeakyRelu','Relu','Sigmoid','Clip','PRelu','QuantizeLinear','DequantizeLinear'):
            o=list(a)
        elif op=='GlobalAveragePool' or op=='ReduceMean':
            o=[1,a[1],1,1]
        elif op in ('Mul','Add','Sub','Div'):
            b=g(ins[1]) or [1]
            # 标量/低维广播：右侧补 1 到 4 维
            bb=[1]*(4-len(b))+list(b) if len(b)<=4 else list(b[-4:])
            aa=list(a) if len(a)==4 else [1]*(4-len(a))+list(a)
            o=[1,max(aa[1],bb[1]),max(aa[2],bb[2]),max(aa[3],bb[3])]
        elif op=='Concat':
            ax=at.get('axis',1); t=0
            for x in ins:
                xd=g(x)
                if xd is None: t=None; break
                t+=xd[ax]
            o=list(a); o[ax]=t
        elif op=='DepthToSpace':
            b=at['blocksize']
            o=[1,a[1]//(b*b),a[2]*b,a[3]*b]
        elif op=='Resize':
            # inputs: X, roi, scales, sizes(optional)
            sc=None; sz=None
            if len(ins)>2 and ins[2] and ins[2] in v: sc=v[ins[2]]
            if len(ins)>3 and ins[3] and ins[3] in v: sz=v[ins[3]]
            if sc and len(sc)>=4:
                o=[1,int(round(a[1]*sc[1])),int(round(a[2]*sc[2])),int(round(a[3]*sc[3]))]
            elif sz and len(sz)>=4:
                o=[int(sz[0]),int(sz[1]),int(sz[2]),int(sz[3])]
            else:
                o=list(a)
        elif op=='Expand':
            tgt=v.get(ins[1])
            o=list(tgt) if tgt else list(a)
        elif op=='Where':
            o=list(g(ins[1]) or a)
        elif op=='ConstantOfShape':
            tgt=v.get(ins[0])
            o=list(tgt) if tgt else [1]
        elif op=='Constant':
            for k2 in ('value',):
                pass
            o=list(a)
        else:
            o=list(a)
        shp[outs[0]]=o
        if verbose: print('  %2d %-14s %-22s -> %s'%(i,op,str(at)[:22],o))
    return shp.get(gout[0]), shp, inits, nodes

if __name__=='__main__':
    import glob,os
    for p in sorted(glob.glob('python/android/sr_qnn/models/*.onnx')):
        if '_q16' in p or 'lowq' in p: continue
        o,_,_,_=infer(p,64)
        print('%-46s 64 -> %s'%(os.path.basename(p),o))
