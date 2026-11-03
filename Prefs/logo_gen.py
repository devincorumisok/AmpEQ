import random
def defs(extra=''):
    return '''<defs>
<linearGradient id="bf" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#B9573A"/><stop offset="1" stop-color="#7F3220"/></linearGradient>
<linearGradient id="bt" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="#E08460"/><stop offset="1" stop-color="#BE5C39"/></linearGradient>
<linearGradient id="bs" x1="0" y1="0" x2="1" y2="0"><stop offset="0" stop-color="#7A2F1B"/><stop offset="1" stop-color="#4E1D0F"/></linearGradient>
<linearGradient id="alu" x1="0" y1="0" x2="1" y2="0"><stop offset="0" stop-color="#8E97A3"/><stop offset="0.35" stop-color="#E9EDF2"/><stop offset="0.6" stop-color="#A9B2BD"/><stop offset="1" stop-color="#6B7480"/></linearGradient>
<linearGradient id="body" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="#FFFFFF"/><stop offset="0.55" stop-color="#E3E7EC"/><stop offset="1" stop-color="#BFC6CF"/></linearGradient>
<linearGradient id="scr" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="#22303F"/><stop offset="1" stop-color="#05080C"/></linearGradient>
<linearGradient id="glare" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="#fff" stop-opacity="0.38"/><stop offset="0.45" stop-color="#fff" stop-opacity="0.08"/><stop offset="0.46" stop-color="#fff" stop-opacity="0"/></linearGradient>
<linearGradient id="bars" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#FF4FA3"/><stop offset="1" stop-color="#36C5F0"/></linearGradient>
<linearGradient id="steel" x1="0" y1="0" x2="1" y2="0"><stop offset="0" stop-color="#5E6874"/><stop offset="0.3" stop-color="#F4F6F9"/><stop offset="0.55" stop-color="#B5BEC9"/><stop offset="1" stop-color="#4C5560"/></linearGradient>
<linearGradient id="steelEnd" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#D5DBE2"/><stop offset="1" stop-color="#7C8692"/></linearGradient>
<linearGradient id="wood" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#E3A86C"/><stop offset="0.45" stop-color="#B97A40"/><stop offset="1" stop-color="#7E4E22"/></linearGradient>
<linearGradient id="grip" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#4A515C"/><stop offset="0.4" stop-color="#2A2F37"/><stop offset="1" stop-color="#121418"/></linearGradient>
<radialGradient id="shadow"><stop offset="0" stop-color="#000" stop-opacity="0.45"/><stop offset="1" stop-color="#000" stop-opacity="0"/></radialGradient>
<filter id="soft" x="-20%" y="-20%" width="140%" height="140%"><feGaussianBlur stdDeviation="1.4"/></filter>
'''+extra+'</defs>'

def brick(x0,y0,L,H,dx,dy):
    f=f'<polygon points="{x0},{y0} {x0+L},{y0} {x0+L},{y0+H} {x0},{y0+H}" fill="url(#bf)"/>'
    s=f'<polygon points="{x0+L},{y0} {x0+L+dx},{y0-dy} {x0+L+dx},{y0-dy+H} {x0+L},{y0+H}" fill="url(#bs)"/>'
    t=f'<polygon points="{x0},{y0} {x0+L},{y0} {x0+L+dx},{y0-dy} {x0+dx},{y0-dy}" fill="url(#bt)"/>'
    hl=f'<polyline points="{x0},{y0} {x0+L},{y0} {x0+L+dx},{y0-dy}" fill="none" stroke="#E89A7A" stroke-width="0.7" opacity="0.55"/>'
    hl+=f'<line x1="{x0+L}" y1="{y0}" x2="{x0+L}" y2="{y0+H}" stroke="#C4694A" stroke-width="0.9" opacity="0.7"/>'
    random.seed(3); pits=''
    for i in range(26):
        px=x0+4+random.random()*(L-8); py=y0+4+random.random()*(H-8)
        pits+=f'<circle cx="{px:.1f}" cy="{py:.1f}" r="{0.6+random.random()*1.2:.1f}" fill="#3B140A" opacity="0.28"/>'
    for i in range(10):
        px=x0+L+3+random.random()*(dx-6); py=y0-dy*((px-x0-L)/dx)+4+random.random()*(H-8)
        pits+=f'<circle cx="{px:.1f}" cy="{py:.1f}" r="{0.5+random.random():.1f}" fill="#000" opacity="0.25"/>'
    return f+s+t+pits+hl

def ipod(x,y,sx,c,d,T=6):
    """iPod touch 7 lying flat, landscape. (x,y)=front-left corner of its bottom on the brick top.
    Proportions from the real device: 123.4 x 58.6 mm body, 4-inch 16:9 screen, home button one end,
    camera + earpiece the other end. Local units: 1 = 1.1 mm."""
    W,Hd=112,53
    mat=lambda dy: f'matrix({sx} 0 {c} {-d} {x} {y+dy})'
    shape=f'<rect x="0" y="0" width="{W}" height="{Hd}" rx="9"/>'
    out=f'<g transform="{mat(2)}" filter="url(#soft)" fill="#000" opacity="0.55">{shape}</g>'
    # curved aluminium edge (real iPod touch 7 sides are rounded): shade each layer by height
    def lerp(c1,c2,t):
        return '#%02X%02X%02X'%tuple(int(c1[i]+(c2[i]-c1[i])*t) for i in range(3))
    stops=[(0,(0x4E,0x56,0x60)),(0.35,(0x9A,0xA3,0xAE)),(0.7,(0xEE,0xF1,0xF5)),(1,(0xB8,0xC0,0xCA))]
    def col(t):
        for (t0,c0),(t1,c1) in zip(stops,stops[1:]):
            if t<=t1: return lerp(c0,c1,(t-t0)/(t1-t0))
        return lerp(*stops[-1][1:],*[0]) if False else '#B8C0CA'
    k=0.0
    while k<=T:
        out+=f'<g transform="{mat(-k)}" fill="{col(k/T)}">{shape}</g>'
        k+=0.25
    # screen 80 x 45 centred; bezels 16 at the ends, 4 at the sides
    sx0,sy0,sw,sh=16,4,80,45
    bars=''.join(f'<rect x="{sx0+6+i*7.6:.1f}" y="{sy0+5}" width="5" height="{h}" rx="1.1"/>' for i,h in enumerate([10,18,27,16,24,13,21,11,15,8]))
    top=f'''<g transform="{mat(-T)}">
 <rect x="0" y="0" width="{W}" height="{Hd}" rx="9" fill="url(#body)"/>
 <rect x="0.7" y="0.7" width="{W-1.4}" height="{Hd-1.4}" rx="8.4" fill="none" stroke="#FFFFFF" stroke-width="1.3" opacity="0.95"/>
 <rect x="0" y="0" width="{W}" height="{Hd}" rx="9" fill="none" stroke="#8D96A1" stroke-width="0.7"/>
 <rect x="{sx0-0.8}" y="{sy0-0.8}" width="{sw+1.6}" height="{sh+1.6}" rx="1.2" fill="#0B0E12"/>
 <rect x="{sx0}" y="{sy0}" width="{sw}" height="{sh}" fill="url(#scr)"/>
 <g fill="url(#bars)">{bars}</g>
 <rect x="{sx0}" y="{sy0}" width="{sw}" height="{sh}" fill="url(#glare)"/>
 <circle cx="{W-8}" cy="{Hd/2}" r="4.8" fill="#E9ECF0" stroke="#9AA3AE" stroke-width="0.9"/>
 <circle cx="{W-8}" cy="{Hd/2}" r="3.9" fill="none" stroke="#fff" stroke-width="0.7" opacity="0.9"/>
 <circle cx="8" cy="{Hd/2}" r="1.6" fill="#1D232B"/>
 <circle cx="7.6" cy="{Hd/2-0.4}" r="0.55" fill="#6C8DB5"/>
</g>'''
    return out+top

def hammer(tx,ty,rot,sc=1.0,motion=True,mirror=False):
    m=''
    if motion:
        m='''<g fill="none" stroke="#9AA6B4" stroke-linecap="round">
 <path d="M-40 -46 Q-44 -20 -36 6" stroke-width="2.6" opacity="0.75"/>
 <path d="M-54 -42 Q-58 -14 -50 12" stroke-width="2" opacity="0.5"/>
 <path d="M-68 -36 Q-72 -8 -64 16" stroke-width="1.5" opacity="0.3"/></g>'''
    return f'''<g transform="translate({tx} {ty}) scale({-sc if mirror else sc} {sc})">{m}<g transform="rotate({rot})">
 <rect x="8" y="-5" width="92" height="10" rx="5" fill="url(#wood)"/>
 <path d="M14 -2.5 Q50 -4 94 -2.6" stroke="#F1C795" stroke-width="1" fill="none" opacity="0.7"/>
 <rect x="72" y="-6.2" width="30" height="12.4" rx="5.5" fill="url(#grip)"/>
 <path d="M76 -3.6 H99" stroke="#6E7682" stroke-width="0.9" opacity="0.7"/>
 <rect x="-12" y="-28" width="24" height="48" rx="3.5" fill="url(#steel)" stroke="#3F4751" stroke-width="0.9"/>
 <rect x="-10" y="-27" width="5" height="46" rx="2" fill="#fff" opacity="0.35"/>
 <rect x="-15" y="18" width="30" height="10" rx="2.6" fill="url(#steelEnd)" stroke="#3F4751" stroke-width="0.9"/>
 <ellipse cx="0" cy="27.6" rx="13" ry="1.6" fill="#EEF2F6" opacity="0.8"/>
 <path d="M-10 -28 L-5 -43 L5 -43 L10 -28 Z" fill="url(#steel)" stroke="#3F4751" stroke-width="0.9"/>
 <rect x="-6" y="-8" width="12" height="8" rx="1.5" fill="#2E343C" opacity="0.5"/>
</g></g>'''

# brick top: front edge y0, depth dy; skew per depth = dx/dy. ipod d=depth scale, c = d*dx/dy
A=f'''<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 280 150" width="280" height="150">{defs()}
<ellipse cx="146" cy="138" rx="112" ry="9" fill="url(#shadow)"/>
{brick(36,100,160,36,42,32)}
{ipod(63.6,97.4,1.0,0.5*42/32,0.5,6)}
{hammer(124.5,45,-26)}
</svg>'''
spark='<g stroke="#FFD24A" stroke-linecap="round" stroke-width="2.2"><line x1="128" y1="56" x2="117" y2="49"/><line x1="125" y1="64" x2="111" y2="65"/><line x1="134" y1="50" x2="130" y2="39"/></g>'
B=f'''<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 280 150" width="280" height="150">{defs()}
<ellipse cx="140" cy="140" rx="118" ry="8" fill="url(#shadow)"/>
{brick(30,104,170,36,50,38)}
{ipod(42,101,1.05,0.6*50/38,0.6,8)}
{spark}
{hammer(150,38,-30,0.9,motion=False,mirror=True)}
</svg>'''
bg='<linearGradient id="bg" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#2B3442"/><stop offset="1" stop-color="#0E1218"/></linearGradient><radialGradient id="spot" cx="0.55" cy="0.35" r="0.7"><stop offset="0" stop-color="#5A6B85" stop-opacity="0.55"/><stop offset="1" stop-color="#000" stop-opacity="0"/></radialGradient>'
C=f'''<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 280 150" width="280" height="150">{defs(bg)}
<rect x="4" y="2" width="272" height="146" rx="26" fill="url(#bg)"/>
<rect x="4" y="2" width="272" height="146" rx="26" fill="url(#spot)"/>
<rect x="4.5" y="2.5" width="271" height="145" rx="25.5" fill="none" stroke="#fff" stroke-opacity="0.12"/>
<ellipse cx="146" cy="134" rx="108" ry="8" fill="url(#shadow)"/>
{brick(40,98,156,32,40,30)}
{ipod(66,94.5,0.96,0.5*40/30,0.5,6)}
{hammer(126,44.5,-26,0.92)}
</svg>'''
for n,s in (('A',A),('B',B),('C',C)): open(f'/tmp/claude-0/logos/logo_{n}.svg','w').write(s)
