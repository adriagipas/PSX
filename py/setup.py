from distutils.core import setup, Extension
import sys,subprocess

def check_version(current,required):
    a= [int(x) for x in current.split('.')]
    b= [int(x) for x in required.split('.')]
    length= min(len(a),len(b))
    for i in range(0,length):
        if a[i]>b[i]: return True
        elif a[i]<b[i]: return False
    return True

def pkgconfig_exe(lib,extra):
    ret= subprocess.check_output(['pkg-config',lib]+extra)
    return ret.decode('utf-8')

def pkgconfig(lib,req_version):
    try:
        version= pkgconfig_exe(lib,['--modversion'])
        if not check_version(version,req_version):
            sys.exit(('%s >= %s not found, current'+
                      ' version is %s')%(lib,req_version,version))
        libs= pkgconfig_exe(lib,['--libs'])
        libs= libs.replace('-l','').split()
        cflags= pkgconfig_exe(lib,['--cflags']).split()
        return libs,cflags
    except Exception as e:
        sys.exit()

glib_libs,glib_cflags= pkgconfig('glib-2.0','2.50')

module= Extension ( 'PSX',
                    sources= [ 'psxmodule.c',
                               '../src/cpu_decode.c',
                               '../src/default_renderer.c',
                               '../src/stats_renderer.c',
                               '../src/gpu.c',
                               '../src/spu.c',
                               '../src/int.c',
                               '../src/mdec.c',
                               '../src/cpu_interpreter.c',
                               '../src/cpu_regs.c',
                               '../src/dma.c',
                               '../src/gte.c',
                               '../src/main.c',
                               '../src/mem.c',
                               '../src/cd.c',
                               '../src/timers.c',
                               '../src/joy.c',
                               'CD/src/crc.c',
                               'CD/src/cue.c',
                               'CD/src/info.c',
                               'CD/src/new.c',
                               'CD/src/utils.c'],
                    depends= [ '../src/PSX.h',
                               '../src/cpu_interpreter_cop0.h',
                               '../src/cpu_interpreter_inst1.h',
                               '../src/cpu_interpreter_cop2.h',
                               'CD/src/CD.h',
                               'CD/src/crc.h',
                               'CD/src/cue.h',
                               'CD/src/utils.h'],
                    libraries= [ 'SDL' ]+glib_libs,
                    extra_compile_args= glib_cflags+['-UNDEBUG'],
                    define_macros= [('__LITTLE_ENDIAN__',None)],
                    include_dirs= [ '../src', 'CD/src' ])

setup ( name= 'PSX',
        version= '1.0',
        description= 'PlayStation simulator',
        ext_modules= [module] )
