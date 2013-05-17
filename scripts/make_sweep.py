sr = 48000
dur = 3
fadein = 0.1
fadeout = 0.03
fmin = 50
fmax = 20000
out_name = 'sweep.wav'

try:
    from pylab import *
except:
    from math import *


# from Aliki - GPL
totalsamps = float(sr * dur)
_k0 = float(sr * fadein)
_k2 = float(sr * fadeout)
_k1 = totalsamps - _k0 - _k2
b = log (fmax / fmin) / _k1;
a = fmin / (b * sr);
r = 0.5 * a * (fmax / fmin) * (_k1 + 0.5 * (_k0 + _k2)) / (b * _k1);
q0 = a * exp (-b * _k0);
sweep = []
for i in range(-_k0, _k1 + _k2):
    if i < 0:   g = cos (0.5 * pi * i / _k0);
    elif i < _k1: g = 1.0;
    else:  g = sin (0.5 * pi * (_k1 + _k2 - i) / _k2);
    q = a * exp (b * i);
    p = q - q0;
    x = g * sin (2 * pi * (p - floor (p)));
    sweep.append(x)

try:
    from scipy.io import wavfile
    wavfile.write(out_name, sr, (array(sweep)*2**15).astype(int16))
except:
    import wave
    import struct
    wav_file = wave.open(out_name, "w")
    comptype = "NONE"
    compname = "not compressed"
    wav_file.setparams((1, 2, sr, len(sweep), comptype, compname))
    for s in sweep:
    # write the audio frames to file
        wav_file.writeframes(struct.pack('h', int(s*(2**15))))
    wav_file.close()
