sr = 48000
dur = 3
fadein = 0.1
fadeout = 0.03
fmin = 20
fmax = 20000
out_name = 'sweep.wav'

# from Aliki - GPL
totalsamps = float(sr * dur)
_k0 = float(48000 * fadein)
_k2 = float(48000 * fadeout)
_k1 = totalsamps - _k0 - _k2
b = log (fmax / fmin) / _k1;
a = fmin / (b * sr);
r = 0.5 * a * (fmax / fmin) * (_k1 + 0.5 * (_k0 + _k2)) / (b * _k1);
q0 = a * exp (-b * _k0);
sweep = []
for i in arange(-_k0, _k1 + _k2):
    if i < 0:   g = cos (0.5 * pi * i / _k0);
    elif i < _k1: g = 1.0;
    else:  g = sin (0.5 * pi * (_k1 + _k2 - i) / _k2);
    q = a * exp (b * i);
    p = q - q0;
    x = g * sin (2 * pi * (p - floor (p)));
    sweep.append(x)
    
from scipy.io import wavfile
wavfile.write(out_name, sr, (array(sweep)*2**15).astype(int16))
