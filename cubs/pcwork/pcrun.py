"""PC-side driver for the hand-over experiments (the Mac used to do this).
usage: python pcrun.py full [--payload hopay2/hofull.payload] [--cookie SYMBOL TAG] [--cmd K|J|A|B] [--wait 240]
Stages the XNU payload, loads the iOS system image from modem_a/modem_b into RAM, jumps (watchdog armed with K),
waits for Android to come back and prints the reboot reasons from the persistent console."""
import argparse, os, struct, subprocess, sys, time
HERE=os.path.dirname(os.path.abspath(__file__))
def adb(*a, inp=None, timeout=None):
    return subprocess.run(['adb']+list(a), input=inp, capture_output=True, text=True, timeout=timeout)
def su(cmd, timeout=None):
    return adb('shell','/data/adb/ksu/bin/ksud debug su', inp=cmd+'\n', timeout=timeout).stdout
def patch_cookie(src, dst, sym, tag):
    va=None
    for l in open(os.path.join(HERE,'kernel.syms')):
        p=l.split()
        if len(p)==3 and p[2]==sym: va=int(p[0],16)
    assert va, sym
    allv=sorted(int(l.split()[0],16) for l in open(os.path.join(HERE,'kernel.syms')) if len(l.split())==3)
    nxt=[x for x in allv if x>va][0]
    assert nxt-va>=36, 'function too small (%d bytes): patch would clobber the next function'%(nxt-va)
    d=bytearray(open(src,'rb').read())
    nsegs=struct.unpack_from('<I',d,12)[0]; off=None
    for i in range(nsegs):
        vm,vs,fo,fs=struct.unpack_from('<QQQQ',d,0x2a8+32*i)
        if vm<=va<vm+fs: off=fo+(va-vm); break
    assert off
    c=0xc0000000|tag; lo,hi=c&0xffff,c>>16
    for k,w in enumerate([0xd2800240,0xf2b08000,0xd2800021,0xf2b00001,0xd2800002|(lo<<5),0xf2a00002|(hi<<5),0xd4000003,0x14000000]):
        struct.pack_into('<I',d,off+4*k,w)
    open(dst,'wb').write(d); print('cookie',hex(c),'at',sym,hex(va))
def patch_lr(src, dst, sym):
    va=None
    for l in open(os.path.join(HERE,'kernel.syms')):
        p=l.split()
        if len(p)==3 and p[2]==sym: va=int(p[0],16)
    assert va, sym
    d=bytearray(open(src,'rb').read())
    nsegs=struct.unpack_from('<I',d,12)[0]; off=None
    for i in range(nsegs):
        vm,vs,fo,fs=struct.unpack_from('<QQQQ',d,0x2a8+32*i)
        if vm<=va<vm+fs: off=fo+(va-vm); break
    assert off
    for k,w in enumerate([0xaa1e03e2,0xd3427442,0xb2620442,0xd2800240,0xf2b08000,0xd2800021,0xf2b00001,0xd4000003,0x14000000]):
        struct.pack_into('<I',d,off+4*k,w)
    open(dst,'wb').write(d); print('LR cookie patch at',sym,hex(va))
def patch_words(src, dst, sym, words):
    va=None
    if sym.startswith('0x'): va=int(sym,16)
    for l in open(os.path.join(HERE,'kernel.syms')):
        p=l.split()
        if len(p)==3 and p[2]==sym: va=int(p[0],16)
    assert va, sym
    d=bytearray(open(src,'rb').read())
    nsegs=struct.unpack_from('<I',d,12)[0]; off=None
    for i in range(nsegs):
        vm,vs,fo,fs=struct.unpack_from('<QQQQ',d,0x2a8+32*i)
        if vm<=va<vm+fs: off=fo+(va-vm); break
    assert off
    for k,w in enumerate(words): struct.pack_into('<I',d,off+4*k,w)
    open(dst,'wb').write(d); print('patched',len(words),'words at',sym,hex(va))
def wait_android(limit):
    t0=time.time(); tgone=None
    while time.time()-t0<limit:
        time.sleep(4)
        try:
            r=adb('shell','getprop sys.boot_completed',timeout=10)
            if r.stdout.strip()!='1' and tgone is None: tgone=time.time()-t0
            if r.stdout.strip()=='1':
                print('seconds until adb answered again: %.0f (first gone at %s)'%(time.time()-t0, 'n/a' if tgone is None else '%.0f'%tgone))
                return True
        except Exception: pass
    return False
def main():
    ap=argparse.ArgumentParser(); ap.add_argument('mode'); ap.add_argument('--payload',default=os.path.join(HERE,'hopay2','hofull.payload'))
    ap.add_argument('--cookie',nargs=2); ap.add_argument('--lr',default=''); ap.add_argument('--words',nargs=2); ap.add_argument('--cmd',default='K'); ap.add_argument('--wait',type=int,default=240); ap.add_argument('--cp',default=''); ap.add_argument('--q',default=''); ap.add_argument('--qk',type=int,default=-1); ap.add_argument('--warm',action='store_true'); ap.add_argument('--noram',action='store_true'); ap.add_argument('--ramoff',default='249c000'); ap.add_argument('--drm',default='')
    a=ap.parse_args()
    pl=a.payload
    if a.words:
        out=os.path.join(HERE,'hopay2','patched.payload'); patch_words(pl,out,a.words[0],[int(x,16) for x in a.words[1].split(',')]); pl=out
    if a.lr:
        out=os.path.join(HERE,'hopay2','patched.payload'); patch_lr(pl,out,a.lr); pl=out
    if a.cookie:
        out=os.path.join(HERE,'hopay2','patched.payload'); patch_cookie(pl,out,a.cookie[0],int(a.cookie[1],0)); pl=out
    if adb('shell','getprop sys.boot_completed').stdout.strip()!='1': print('phone not in Android'); sys.exit(2)
    adb('push',os.path.join(HERE,'handover','cubs_stage.ko'),'/data/local/tmp/cubs_stage.ko')
    if a.drm:
        adb('push',os.path.join(HERE,'drmfb','drmfb'),'/data/local/tmp/drmfb'); adb('push',os.path.join(HERE,'drmfb_run.sh'),'/data/local/tmp/drmfb_run.sh'); adb('shell','input keyevent KEYCODE_WAKEUP; svc power stayon true')
    adb('shell','sync')
    adb('push',pl,'/data/local/tmp/ho.payload')
    drm_pre=''
    drm_ld=''
    if a.drm:
        NL=chr(10)
        drm_pre='for d in e500000.pinctrl da2c000.spi; do echo on > /sys/devices/platform/$d/power/control; done'+NL+'sh /data/local/tmp/drmfb_run.sh'+NL
        drm_ld=('printf O%s > /dev/cubs_go'+NL+'cat /data/local/tmp/fblist.bin > /dev/cubs_ram'+NL+'printf L%s:2000 > /dev/cubs_go'+NL) % (a.drm, a.drm)
    cpline=('printf P%x > /dev/cubs_go' % int(a.cp,0)+chr(10)) if a.cp else ''
    BS=chr(92)
    qline=("printf 'Q%s' > /dev/cubs_go" % a.q + chr(10)) if a.q else ''
    if a.warm:
        qline="printf 'Q"+BS+"376"+BS+"001"+BS+"001"+BS+"001"+BS+"001"+BS+"001' > /dev/cubs_go"+chr(10)
    if a.qk>=0:
        qline="printf 'Q"+BS+"377"+BS+"%03o"%(a.qk&255)+BS+"%03o"%((a.qk>>8)&255)+BS+"001"+BS+"001"+BS+"001' > /dev/cubs_go"+chr(10)
    ramline='true' if a.noram else '(cat /dev/block/by-name/modem_a /dev/block/by-name/modem_b | head -c 2492203008) > /dev/cubs_ram'
    script=f'''{drm_pre}rm -f /dev/cubs_stage /dev/cubs_go /dev/cubs_ram; rmmod cubs_stage 2>/dev/null
insmod /data/local/tmp/cubs_stage.ko; sleep 2
cat /data/local/tmp/ho.payload > /dev/cubs_stage
{cpline}{qline}taskset 1 sh -c "echo S > /dev/cubs_go"
{drm_ld}printf O{a.ramoff} > /dev/cubs_go
{ramline}
echo C > /dev/cubs_go
dmesg | grep cubs_stage | tail -4
for c in 1 2 3 4 5 6; do echo 0 > /sys/devices/system/cpu/cpu$c/online; done
sync; echo JUMPING
taskset 1 sh -c "echo {a.cmd} > /dev/cubs_go"
'''
    open(os.path.join(HERE,'fb_dev.sh'),'w',newline='\n').write(script)
    adb('push',os.path.join(HERE,'fb_dev.sh'),'/data/local/tmp/fb_dev.sh')
    try: print(su('sh /data/local/tmp/fb_dev.sh',timeout=120)[-400:])
    except subprocess.TimeoutExpired: print('(device went away, expected after the jump)')
    ok=wait_android(a.wait); print('android back:',ok)
    if ok:
        time.sleep(15)
        print(su('grep -a "Reboot reason" /sys/fs/pstore/console-ramoops-0 | tail -3'))
        print(su('head -c 6000 /sys/fs/pstore/console-ramoops-0 | strings | head -50 | cut -c1-160'))
        print(su('grep -a -n "iOS\|launchd\|CUBS\|panic" /sys/fs/pstore/console-ramoops-0 | grep -v "init:\|servicemanager" | head -12 | cut -c1-200'))
main()
