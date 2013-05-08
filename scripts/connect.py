
import os

#------------------------------
src = [i + 15 for i in range(2)]
dest = [ i for i in range(2)]

src_text = [ 'system:capture_%s'%s for s in src]
dest_text = [ 'ir_rec:input_%s'%s for s in dest]

for s, d in zip(src_text, dest_text):
    os.system('jack_connect %s %s'%(s,d))

#------------------------------
src = [i + 12 for i in range(30)]
dest = [ i + 17 for i in range(30)]

src_text = [ 'ir_rec:output_%s'%s for s in src]
dest_text = [ 'system:playback_%s'%s for s in dest]


for s, d in zip(src_text, dest_text):
    os.system('jack_connect %s %s'%(s,d))
#-------------------------------

src = [i for i in range(12)]
dest = [ i + 1 for i in range(12)]

src_text = [ 'ir_rec:output_%s'%s for s in src]
dest_text = [ 'system:playback_%s'%s for s in dest]

for s, d in zip(src_text, dest_text):
    os.system('jack_connect %s %s'%(s,d))
#------------------------------
src = [i + 42 for i in range(12)]
dest = [ i + 49 for i in range(12)]

src_text = [ 'ir_rec:output_%s'%s for s in src]
dest_text = [ 'system:playback_%s'%s for s in dest]

for s, d in zip(src_text, dest_text):
    os.system('jack_connect %s %s'%(s,d))