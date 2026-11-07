import subprocess,sys,re
V={'pc':'f9408423,d3427463,b2620462,d2800240,f2b08000,d2800021,f2b00001,d4000003,14000000',
   'far':'f9408c23,d3427463,b2620462,d2800240,f2b08000,d2800021,f2b00001,d4000003,14000000',
   'esr':'b9412023,53027c63,b2620462,d2800240,f2b08000,d2800021,f2b00001,d4000003,14000000',
   'lr':'f9407c23,d3427463,b2620462,d2800240,f2b08000,d2800021,f2b00001,d4000003,14000000'}
what=sys.argv[1]; tries=int(sys.argv[2]) if len(sys.argv)>2 else 5
sym=sys.argv[3] if len(sys.argv)>3 else '_panic_with_thread_kernel_state'
for t in range(tries):
    out=subprocess.run([sys.executable,'pcrun.py','full','--words',sym,V[what],'--cmd','K','--wait','200']+sys.argv[4:],capture_output=True,text=True).stdout
    reasons=re.findall(r'Reboot reason: (0x[0-9a-f]+)',out)
    new=reasons[-2] if len(reasons)>=2 else None
    print(t,reasons[-3:])
    # a fresh pair is (0xcXXXXXXX, 0x4XXXXXXX); the first of the last pair is ours
    if len(reasons)>=2 and reasons[-1].startswith('0x4') and not reasons[-2].startswith('0xcb') :
        v=int(reasons[-2],16)&0x3fffffff
        if what=='esr': print('ESR =',hex(v<<2))
        else: print(what,'= 0xfffffff0'+'%08x'%(v<<2) if v<<2 < 0x40000000 else hex(v<<2))
        break
