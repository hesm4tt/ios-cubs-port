import subprocess,sys,struct
from elftools.elf.elffile import ELFFile
ZIG=[sys.executable,'-m','ziglang']
T='-target'
def run(args):
    r=subprocess.run(args,capture_output=True,text=True)
    if r.returncode: print(r.stdout,r.stderr); sys.exit(1)
VM="6.12.69-android16-6-g5c5f2fea42dd-ab15835541-4k SMP preempt mod_unload modversions aarch64"
run(ZIG+['cc','-target','aarch64-freestanding-none','-c','blobs.S','-o','blobs.o'])
f=ELFFile(open('blobs.o','rb'))
text=f.get_section_by_name('.text').data()
sym={s.name:s['st_value'] for s in f.get_section_by_name('.symtab').iter_symbols()}
def arr(name,b):
    w=struct.unpack('<%dI'%(len(b)//4),b)
    return 'static const u32 %s[] = {\n\t%s\n};\n#define %s_WORDS %d\n'%(name,',\n\t'.join('0x%08x'%x for x in w),name.upper(),len(w))
open('blobs.h','w').write(arr('stub_code',text[sym['stub_start']:sym['stub_end']])+arr('tramp_code',text[sym['tramp_start']:sym['tramp_end']])+arr('amp_code',text[sym['amp_start']:sym['amp_end']]))
run(ZIG+['cc','-target','aarch64-freestanding-none','-O2','-g0','-ffreestanding','-fno-builtin','-fno-PIC','-fno-pie','-fno-stack-protector','-mno-outline-atomics','-fsanitize=kcfi','-fsanitize-cfi-icall-experimental-normalize-integers','-ffixed-x18','-c','cubs_stage.c','-o','cubs_stage.o'])
run(ZIG+['cc','-target','aarch64-freestanding-none','-c','-DVERMAGIC="vermagic=%s"'%VM,'modmeta_stage.S','-o','modmeta_stage.o'])
run(ZIG+['ld.lld','-r','-o','cubs_stage.ko','cubs_stage.o','modmeta_stage.o'])
import os; print('built',os.path.getsize('cubs_stage.ko'))
