import subprocess,sys,re
# read the tail of the XNU log, 4 chars per run: python readlog.py <first k> <last k> [payload]
a=int(sys.argv[1]); b=int(sys.argv[2]); pl=sys.argv[3] if len(sys.argv)>3 else 'hopay2/hofull_nort.payload'
res={}
for k in range(a,b+1):
    out=subprocess.run([sys.executable,'pcrun.py','full','--payload',pl,'--qk',str(k),'--cmd','K','--wait','200'],capture_output=True,text=True).stdout
    r=re.findall(r'Reboot reason: (0x[0-9a-f]+)',out)
    v=None
    for x in r[-3:]:
        n=int(x,16)
        if (n>>28)==0xc and (n&0x0fffffff)!=0x0 : v=n&0x0fffffff
    if len(r)>=2 and r[-1].startswith('0x4'): v=int(r[-2],16)&0x0fffffff
    txt=''.join(chr((v>>s)&0x7f) if v is not None and 32<=((v>>s)&0x7f)<127 or (v is not None and ((v>>s)&0x7f) in (10,13)) else '.' for s in (21,14,7,0)) if v is not None else '????'
    print(k,hex(v) if v is not None else None,repr(txt),flush=True)
