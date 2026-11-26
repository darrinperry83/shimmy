"""Redirect two KDK locore imports; preserve all instruction/relocation bytes."""
import hashlib
from pathlib import Path
import shutil
import struct
import subprocess

ARCHIVE_SHA='15880a9a5433d161afe1e6c3fec7ae48053fb1d57ac53f60849d6d0da4fdbbcb'
LOCORE_SHA='8a2c05e147337b2aaa37ad2057fafd5991314abada02400bf10266e4faa7f7ea'
IMPORTS={b'_ml_sign_thread_state':b'_rd_linux_entry_sign',
         b'_ml_check_signed_state':b'_rd_linux_entry_check'}

def redirect(data):
    """Change nlist string indexes only, never symbol/relocation indexes."""
    if hashlib.sha256(data).hexdigest()!=LOCORE_SHA:
        raise ValueError('unqualified KDK locore object')
    out=bytearray(data)
    magic,cpu,_,kind,count,size,_,_=struct.unpack_from('<8I',out)
    if (magic,cpu,kind)!=(0xfeedfacf,0x100000c,1):raise ValueError('expected ARM64 Mach-O object')
    offset=32;symtab=None
    for _ in range(count):
        command,length=struct.unpack_from('<II',out,offset)
        if length<8 or offset+length>32+size:raise ValueError('invalid commands')
        if command==2:
            if symtab is not None or length!=24:raise ValueError('unexpected symbol table')
            symtab=offset
        offset+=length
    if offset!=32+size or symtab is None:raise ValueError('missing symbols')
    _,_,symbols,number,strings,string_size=struct.unpack_from('<6I',out,symtab)
    if symbols+number*16>strings or strings+string_size!=len(out):raise ValueError('unbounded symbol table')
    original=bytes(out[strings:]);new_strings=bytearray(original);seen=set();allowed=set(range(symtab+20,symtab+24))
    for i in range(number):
        entry=symbols+i*16
        index,type_,section,desc,value=struct.unpack_from('<IBBHQ',out,entry)
        if index>=len(original):raise ValueError('invalid symbol name')
        end=original.find(b'\0',index)
        if end<0:raise ValueError('unterminated symbol')
        name=original[index:end]
        if name not in IMPORTS:continue
        if type_!=1 or section or value or name in seen:raise ValueError('expected one undefined external per helper')
        seen.add(name)
        struct.pack_into('<I',out,entry,len(new_strings))
        allowed.update(range(entry,entry+4))
        new_strings.extend(IMPORTS[name]+b'\0')
    if seen!=IMPORTS.keys():raise ValueError('missing locore helper import')
    new_strings.extend(b'\0' * (-(strings + len(new_strings)) % 8))
    struct.pack_into('<I',out,symtab+20,len(new_strings))
    out[strings:]=new_strings
    if any(a!=b and i not in allowed for i,(a,b) in enumerate(zip(data,out))):
        raise ValueError('changed bytes outside symbol-name metadata')
    return bytes(out)

def bridge_archive(target,kdk):
    source=kdk/'System/Library/KernelSupport/libVMAPPLE.os.DEVELOPMENT.a'
    if hashlib.sha256(source.read_bytes()).hexdigest()!=ARCHIVE_SHA:
        raise ValueError('unqualified KDK support archive')
    members=subprocess.check_output(['xcrun','ar','-t',str(source)],text=True).splitlines()
    members=[n for n in members if not n.startswith('__.SYMDEF')]
    if members.count('locore.o')!=1:raise ValueError('expected one locore')
    original=subprocess.check_output(['xcrun','ar','-p',str(source),'locore.o'])
    modified=redirect(original)
    folder=target/'kdk-bridge';folder.mkdir(exist_ok=True)
    (folder/'locore.o').write_bytes(modified)
    destination=target/'libVMAPPLE.linux-entry.a';shutil.copy2(source,destination)
    subprocess.run(['xcrun','ar','-r',str(destination),str(folder/'locore.o')],check=True)
    actual=subprocess.check_output(['xcrun','ar','-t',str(destination)],text=True).splitlines()
    if [n for n in actual if not n.startswith('__.SYMDEF')]!=members:raise ValueError('changed archive membership')
    for name in members:
        expected=modified if name=='locore.o' else subprocess.check_output(['xcrun','ar','-p',str(source),name])
        if subprocess.check_output(['xcrun','ar','-p',str(destination),name])!=expected:
            raise ValueError(f'unexpected member change: {name}')
    return destination,dict(original_sha256=ARCHIVE_SHA,modified_sha256=hashlib.sha256(destination.read_bytes()).hexdigest(),
        locore_original_sha256=LOCORE_SHA,locore_modified_sha256=hashlib.sha256(modified).hexdigest(),
        redirected_imports={a.decode():b.decode() for a,b in IMPORTS.items()},
        instruction_and_relocation_bytes_unchanged=True,other_members_byte_identical=True)


def verify_linked(kernel):
    """Check resolved call sites in the final kernel, rather than just archive intent."""
    symbols=subprocess.check_output(['xcrun','nm','-n',str(kernel)],text=True)
    bases=[int(line.split()[0],16) for line in symbols.splitlines() if line.endswith(' _ExceptionVectorsBase')]
    if len(bases)!=1:raise ValueError('missing/duplicate exception vector base')
    evidence={}
    for offset,name in ((0x1de8,'_rd_linux_entry_sign'),(0x22f8,'_rd_linux_entry_check')):
        address=bases[0]+offset
        text=subprocess.check_output(['xcrun','llvm-objdump','-d','--no-show-raw-insn',
            f'--start-address={hex(address)}',f'--stop-address={hex(address+4)}',str(kernel)],text=True)
        if f'<{name}>' not in text or '\tbl\t' not in text:
            raise ValueError(f'KDK entry call did not resolve to {name}')
        evidence[hex(offset)]=name
    wrappers=subprocess.check_output(['xcrun','llvm-objdump',
        '--disassemble-symbols=_rd_linux_entry_sign,_rd_linux_entry_check',
        '--no-show-raw-insn',str(kernel)],text=True)
    if '<_ml_sign_thread_state>' not in wrappers or '<_ml_check_signed_state>' not in wrappers:
        raise ValueError('original Apple helper delegation missing')
    return dict(entry_calls=evidence,original_apple_helpers_retained=True)
