import PSX
import sys

if len(sys.argv)!=3:
    sys.exit('%s <BIOS> <CDIMG>'%sys.argv[0])
bios_fn= sys.argv[1]
cd_fn= sys.argv[2]

PSX.init(open(bios_fn,'rb').read())
PSX.set_disc(cd_fn)
PSX.loop()
PSX.close()
