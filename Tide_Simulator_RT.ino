#include <WiFi.h>
#include <WebServer.h>

// Set your Access Point credentials
const char* ssid = "Tide Simulator";
const char* password = "tidesimulator"; // Must be at least 8 characters

// Initialize the web server on port 80
WebServer server(80);

// ==============================================================================
// Ray-traced WebGL tides simulation (runs entirely on the CLIENT's GPU).
// The ESP32 only serves this page from flash.
// ==============================================================================
const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>Gravitational Tides - Ray Traced</title>
<style>
  :root{
    --bg:#02040a; --panel:rgba(14,18,28,.72); --border:rgba(120,160,255,.22);
    --accent:#6cb4ff; --text:#e8eef9; --muted:#8a96ab;
  }
  *{box-sizing:border-box}
  body{
    font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,Helvetica,Arial,sans-serif;
    background:radial-gradient(circle at 50% 30%,#0b1226 0%,#02040a 70%);
    color:var(--text); margin:0; padding:18px; min-height:100vh;
    display:flex; flex-direction:column; align-items:center; text-align:center;
  }
  h1{font-weight:200; letter-spacing:6px; text-transform:uppercase; font-size:1.25em; margin:6px 0 16px}
  h1 small{display:block; font-size:.5em; letter-spacing:3px; color:var(--muted); margin-top:6px}
  .stage{
    position:relative; width:100%; max-width:1100px; border-radius:14px; overflow:hidden;
    border:1px solid var(--border); background:#000;
    box-shadow:0 0 0 1px rgba(255,255,255,.03),0 20px 60px rgba(0,0,0,.9),0 0 80px rgba(60,110,255,.12);
  }
  canvas{display:block; width:100%; height:auto}
  .hud{
    position:absolute; left:10px; top:10px; padding:6px 10px; border-radius:8px;
    background:rgba(0,0,0,.55); border:1px solid var(--border); font:11px/1.5 monospace;
    color:#9fd0ff; text-align:left; pointer-events:none; backdrop-filter:blur(6px);
  }
  .panel{
    margin-top:20px; padding:22px 30px; width:100%; max-width:640px;
    background:var(--panel); border:1px solid var(--border); border-radius:14px;
    backdrop-filter:blur(14px); -webkit-backdrop-filter:blur(14px);
    box-shadow:0 10px 40px rgba(0,0,0,.6);
  }
  .row{display:flex; justify-content:space-between; align-items:center; margin-bottom:10px}
  .badge{
    background:rgba(108,180,255,.14); color:var(--accent); padding:4px 14px; border-radius:20px;
    font-family:monospace; font-size:1.2em; font-weight:700; border:1px solid rgba(108,180,255,.35);
  }
  input[type=range]{width:100%; -webkit-appearance:none; background:transparent; margin:10px 0 18px}
  input[type=range]:focus{outline:none}
  input[type=range]::-webkit-slider-runnable-track{height:6px; border-radius:4px; background:linear-gradient(90deg,#1d2a44,#2f4a7d,#1d2a44)}
  input[type=range]::-webkit-slider-thumb{
    -webkit-appearance:none; height:22px; width:22px; border-radius:50%; margin-top:-8px; cursor:pointer;
    background:radial-gradient(circle at 35% 30%,#fff,#6cb4ff 55%,#2a6fd6); box-shadow:0 0 14px rgba(108,180,255,.8);
  }
  input[type=range]::-moz-range-track{height:6px; border-radius:4px; background:#2a3a5c}
  input[type=range]::-moz-range-thumb{height:22px; width:22px; border:none; border-radius:50%; background:#6cb4ff}
  .opts{display:flex; gap:18px; justify-content:center; align-items:center; flex-wrap:wrap; margin:4px 0 14px; color:var(--muted); font-size:.9em}
  select{background:#0d1424; color:var(--text); border:1px solid var(--border); border-radius:6px; padding:4px 8px}
  .status{font-weight:500; font-size:1.15em; padding-top:12px; border-top:1px solid var(--border)}
  .err{color:#ff8b8b; padding:20px; display:none}
</style>
</head>
<body>
<h1>Gravitational Tides<small>Real-time path tracing &bull; WebGL</small></h1>

<div class="stage">
  <canvas id="gl" width="1920" height="1080"></canvas>
  <div class="hud" id="hud">initialising...</div>
  <div class="err" id="err"></div>
</div>

<div class="panel">
  <div class="row">
    <span>Moon Orbital Position</span>
    <span class="badge"><span id="deg">0</span>&deg;</span>
  </div>
  <input type="range" id="moon" min="0" max="360" step="0.5" value="0">
  <div class="opts">
    <label><input type="checkbox" id="auto"> Auto-orbit</label>
    <label>Resolution
      <select id="res">
        <option value="720">720p</option>
        <option value="1080" selected>1080p</option>
        <option value="1440">1440p</option>
        <option value="2160">4K (extreme)</option>
      </select>
    </label>
  </div>
  <div class="status" id="status">Spring Tide (Aligned: Maximum Range)</div>
</div>

<script id="vs" type="x-shader/x-vertex">
attribute vec2 a;
void main(){ gl_Position = vec4(a, 0.0, 1.0); }
</script>

<script id="fs" type="x-shader/x-fragment">
precision highp float;
uniform vec2  uRes;
uniform float uTime;
uniform float uMoon;   // moon orbital angle (rad)
uniform float uRot;    // earth spin (rad)

#define PI 3.14159265
const float MOON_ORBIT = 3.6;
const float MOON_R     = 0.34;
const vec3  L          = vec3(1.0, 0.0, 0.0);   // direction to the Sun

vec3 MD;     // unit vector to the Moon
vec3 MC;     // moon centre
mat3 ROT;    // earth spin
vec2 gP;     // screen coord (for sun glow)

// ------------------------------------------------------------------ noise
float hash13(vec3 p){ p = fract(p*.1031); p += dot(p, p.zyx+31.32); return fract((p.x+p.y)*p.z); }
vec3  hash33(vec3 p){ p = fract(p*vec3(.1031,.1030,.0973)); p += dot(p, p.yxz+33.33); return fract((p.xxy+p.yxx)*p.zyx); }

float noise(vec3 x){
  vec3 i = floor(x), f = fract(x);
  f = f*f*f*(f*(f*6.0-15.0)+10.0);
  return mix(mix(mix(hash13(i),                  hash13(i+vec3(1.,0.,0.)), f.x),
                 mix(hash13(i+vec3(0.,1.,0.)),   hash13(i+vec3(1.,1.,0.)), f.x), f.y),
             mix(mix(hash13(i+vec3(0.,0.,1.)),   hash13(i+vec3(1.,0.,1.)), f.x),
                 mix(hash13(i+vec3(0.,1.,1.)),   hash13(i+vec3(1.,1.,1.)), f.x), f.y), f.z);
}

const mat3 M3 = mat3(0.00,0.80,0.60, -0.80,0.36,-0.48, -0.60,-0.48,0.64);

float fbm5(vec3 p){
  float a=.5, s=0., n=0.;
  for(int i=0;i<5;i++){ s+=a*noise(p); n+=a; p=M3*p*2.02; a*=.5; }
  return s/n;
}
float fbm8(vec3 p){
  float a=.5, s=0., n=0.;
  for(int i=0;i<8;i++){ s+=a*noise(p); n+=a; p=M3*p*2.03; a*=.5; }
  return s/n;
}

mat3 rotY(float a){ float c=cos(a), s=sin(a); return mat3(c,0.,-s, 0.,1.,0., s,0.,c); }

// ------------------------------------------------------------- tide + earth
float tide(vec3 n){
  float l = dot(n, MD);     // lunar bulge (both sides)
  float s = n.x;            // solar bulge (both sides)
  return 0.10*l*l + 0.043*s*s;
}
float landR(vec3 q){ return 1.0 + 0.45*(fbm5(q*1.8) - 0.54); }
float S(vec3 n){ return max(landR(ROT*n), 1.0 + tide(n)); }
float F(vec3 p){ return length(p) - S(normalize(p)); }

vec3 calcNormal(vec3 p){
  vec2 e = vec2(1.,-1.)*0.0025;
  return normalize(e.xyy*F(p+e.xyy) + e.yyx*F(p+e.yyx) + e.yxy*F(p+e.yxy) + e.xxx*F(p+e.xxx));
}

float cloudAt(vec3 n){
  vec3 cq = rotY(uRot*1.3 + uTime*0.02)*n;
  float c = fbm8(cq*2.6 + vec3(1.7,0.3,2.1));
  return smoothstep(0.50, 0.72, c);
}

// soft sphere occlusion toward the light
float sphShadow(vec3 ro, vec3 rd, vec3 c, float r, float k){
  vec3 oc = ro - c;
  float b = dot(oc, rd);
  if(b > 0.0) return 1.0;
  float d = length(oc - rd*b);
  float pen = 0.01 + k*(-b);
  return clamp(0.5 + 0.5*(d - r)/pen, 0.0, 1.0);
}

// terrain / ocean-bulge self shadow (ray marched against the real surface)
float terrainShadow(vec3 p, vec3 n0){
  if(dot(n0, L) < 0.0) return 0.0;
  float res = 1.0, t = 0.015;
  for(int i=0;i<30;i++){
    vec3 q = p + n0*0.004 + L*t;
    float f = F(q);
    res = min(res, 10.0*f/t);
    t += clamp(f, 0.015, 0.2);
    if(res < 0.01 || t > 1.6) break;
  }
  return clamp(res, 0.0, 1.0);
}

vec3 shadeEarth(vec3 p, vec3 rd){
  vec3 n0 = normalize(p);
  vec3 n  = calcNormal(p);
  vec3 q  = ROT*n0;
  float hh = fbm5(q*1.8);
  float lR = 1.0 + 0.45*(hh - 0.54);
  float oR = 1.0 + tide(n0);
  float depth = oR - lR;
  bool water = depth > 0.0;
  float ndl0 = dot(n0, L);
  float lat = abs(q.y);

  vec3 albedo; vec3 bn = n; float spec = 0.0;

  if(water){
    vec3 wp = p*46.0;
    vec3 wv = vec3(noise(wp+vec3(uTime*.8,0.,uTime*.5)),
                   noise(wp+vec3(13.,uTime*.7,3.)),
                   noise(wp+vec3(7.,5.,-uTime*.9))) - 0.5;
    bn = normalize(n + wv*0.20);
    albedo = mix(vec3(0.02,0.52,0.56), vec3(0.0,0.05,0.22), 1.0-exp(-depth*22.0));
    float foam = (1.0-smoothstep(0.0,0.018,depth))*(0.5+0.5*noise(p*90.0+uTime*1.5));
    albedo = mix(albedo, vec3(0.92), foam*0.8);
    spec = 1.0;
  } else {
    vec3 dq = q*16.0;
    float d0 = fbm8(dq);
    vec3 g = vec3(fbm8(dq+ROT*vec3(0.04,0.,0.)) - d0,
                  fbm8(dq+ROT*vec3(0.,0.04,0.)) - d0,
                  fbm8(dq+ROT*vec3(0.,0.,0.04)) - d0)/0.04;
    bn = normalize(n - 0.11*(g - n*dot(g,n)));

    float moist = fbm5(q*4.2 + vec3(3.,1.,7.));
    float det   = fbm5(q*26.0);
    vec3 forest = vec3(.04,.20,.05), grass = vec3(.18,.33,.08);
    vec3 desert = vec3(.62,.50,.28), rock  = vec3(.33,.28,.25);
    albedo = mix(grass, forest, smoothstep(.42,.62,moist));
    albedo = mix(desert, albedo, smoothstep(.36,.46,moist));
    albedo = mix(albedo, rock, smoothstep(.60,.72,hh));
    float snow = max(smoothstep(.80,.90,lat+(det-.5)*.25), smoothstep(.70,.78,hh));
    albedo = mix(albedo, vec3(.93,.95,1.0), snow);
    albedo *= 0.7 + 0.6*det;
    albedo = mix(vec3(.75,.68,.45), albedo, smoothstep(0.0,0.012,lR-oR));
  }
  float ice = smoothstep(.84,.92, lat + (noise(q*20.)-.5)*.1);
  albedo = mix(albedo, vec3(.90,.95,1.0), ice);
  spec *= 1.0 - ice;

  // ---- ray traced shadows
  float moonSh = sphShadow(p, L, MC, MOON_R, 0.05);
  float ter    = terrainShadow(p, n0);
  float sh     = moonSh*ter;

  float cl = cloudAt(n0);
  float cs = cloudAt(normalize(n0 + L*0.06));   // cloud shadow on the ground

  float ndl = dot(bn, L);
  float dif = max(ndl, 0.0)*smoothstep(-0.02, 0.10, ndl0);
  vec3 sunCol = vec3(1.0,0.93,0.82)*2.6;
  vec3 amb = vec3(.012,.022,.05) + vec3(.6,.22,.08)*0.12*exp(-pow(ndl0*7.0,2.0));

  vec3 col = albedo*(dif*sh*(1.0-0.55*cs)*sunCol + amb);

  // water glint + fresnel
  vec3 H = normalize(L - rd);
  float sp = pow(max(dot(bn,H),0.0),260.0)*9.0 + pow(max(dot(bn,H),0.0),30.0)*0.4;
  col += spec*sp*sh*smoothstep(0.0,0.1,ndl0)*(1.0-cl)*vec3(1.0,0.9,0.75);
  float fr = pow(1.0 - max(dot(bn,-rd),0.0), 4.0)*0.5;
  col += spec*fr*vec3(.25,.45,.8)*max(ndl0,0.0)*(1.0-cl)*moonSh;

  // night-side city lights
  if(!water){
    float night = 1.0 - smoothstep(-0.12, 0.06, ndl0);
    float city = smoothstep(.62,.74,fbm5(q*20.0))*smoothstep(.55,.62,hh)*(1.0-smoothstep(.55,.8,lat));
    col += vec3(1.0,0.72,0.35)*city*night*(1.0-cl)*1.6;
  }

  // clouds
  vec3 cloudLit = sunCol*smoothstep(-0.05,0.25,ndl0)*0.85*moonSh + vec3(.02,.03,.05)
                + vec3(.9,.35,.12)*exp(-pow(ndl0*8.0,2.0))*0.5;
  col = mix(col, cloudLit, cl*0.92);
  return col;
}

// ----------------------------------------------------------------- moon
float craterH(vec3 p){
  vec3 i = floor(p), f = fract(p);
  float h = 0.0;
  for(int x=-1;x<=1;x++) for(int y=-1;y<=1;y++) for(int z=-1;z<=1;z++){
    vec3 g = vec3(float(x),float(y),float(z));
    vec3 o = hash33(i+g);
    vec3 d = g + 0.15 + o*0.7 - f;
    float rad = 0.18 + 0.28*hash13(i+g+17.0);
    float k = length(d)/rad;
    float bowl = (min(k*k,1.0)-1.0)*0.5;
    float rim  = exp(-pow((k-1.05)*4.5,2.0))*0.22;
    h += bowl + rim;
  }
  return h;
}
float moonHeight(vec3 q){
  return 0.50*craterH(q*3.2) + 0.35*craterH(q*7.5+5.0) + 0.20*craterH(q*17.0+11.0) + 0.05*fbm5(q*30.0);
}

vec3 shadeMoon(vec3 p, vec3 rd){
  vec3 n0 = normalize(p - MC);
  mat3 RM = rotY(uMoon);          // tidally locked: same face toward Earth
  vec3 q = RM*n0;
  float h0 = moonHeight(q);
  float e = 0.02;
  vec3 g = vec3(moonHeight(q+RM*vec3(e,0.,0.)) - h0,
                moonHeight(q+RM*vec3(0.,e,0.)) - h0,
                moonHeight(q+RM*vec3(0.,0.,e)) - h0)/e;
  vec3 bn = normalize(n0 - 0.30*(g - n0*dot(g,n0)));

  float mar  = smoothstep(.46,.56, fbm5(q*1.5+vec3(3.,1.,2.)));
  float fine = fbm8(q*45.0);
  vec3 alb = mix(vec3(.60,.58,.55), vec3(.20,.20,.22), mar)*(0.75+0.5*fine);
  alb += vec3(.12)*smoothstep(.2,.45,h0)*(1.0-mar);

  float esh = sphShadow(p, L, vec3(0.0), 1.12, 0.05);   // Earth's shadow on the Moon
  float dif = max(dot(bn,L),0.0)*smoothstep(0.0,0.05,dot(n0,L));
  float toE = max(dot(n0,-MD),0.0);
  float earthLit = 0.5*(1.0 + dot(L,MD));
  vec3 col = alb*(dif*esh*vec3(1.0,0.97,0.90)*2.6 + vec3(.05,.07,.12)*toE*earthLit);
  col += alb*vec3(.6,.15,.05)*0.25*(1.0-esh)*toE*smoothstep(0.0,0.1,dot(n0,L)+0.3);  // blood-moon glow
  return col;
}

// ----------------------------------------------------------- atmosphere
vec3 atmosphere(vec3 ro, vec3 rd, float tMax){
  const float RA = 1.26;
  float b = dot(ro,rd), c = dot(ro,ro) - RA*RA, h = b*b - c;
  if(h < 0.0) return vec3(0.0);
  h = sqrt(h);
  float t0 = max(-b-h, 0.0), t1 = min(-b+h, tMax);
  if(t1 <= t0) return vec3(0.0);
  float dt = (t1-t0)/14.0;
  float mu = dot(rd,L);
  float ray = 0.75*(1.0+mu*mu);
  float g = 0.76;
  float mie = (1.0-g*g)/pow(1.0+g*g-2.0*g*mu, 1.5)*0.08;
  vec3 sum = vec3(0.0);
  for(int i=0;i<14;i++){
    vec3 p = ro + rd*(t0 + (float(i)+0.5)*dt);
    float r = length(p);
    float dens = exp(-max(r-1.0,0.0)*11.0);
    float pl = dot(p,L);
    float perp = length(p - L*pl);
    float vis = (pl < 0.0) ? smoothstep(0.97,1.06,perp) : 1.0;
    float ls = dot(normalize(p), L);
    vec3 sc = mix(vec3(.16,.40,1.0), vec3(1.0,.38,.10), 1.0-smoothstep(-0.05,0.45,ls));
    sum += dens*dt*vis*(sc*ray*0.9 + vec3(1.0,0.8,0.6)*mie);
  }
  return sum*2.4;
}

// ----------------------------------------------------------- background
vec3 background(vec3 rd){
  vec3 col = vec3(0.004,0.006,0.014);
  vec3 mwN = normalize(vec3(.25,.9,-.35));
  float band = exp(-pow(dot(rd,mwN)*3.2, 2.0));
  float mw = fbm5(rd*5.0+3.0);
  col += vec3(.10,.12,.20)*band*(0.3 + 1.2*mw*mw);
  for(int l=0;l<3;l++){
    float sc = 60.0 + float(l)*55.0;
    vec3 sp = rd*sc, id = floor(sp), f = fract(sp)-0.5;
    vec3 o = (hash33(id)-0.5)*0.7;
    float d = length(f-o);
    float h = hash13(id + float(l)*11.0);
    float sz = 0.06 + 0.12*h*h;
    float tw = 0.7 + 0.3*sin(uTime*(2.0+h*5.0)+h*60.0);
    vec3 tint = mix(vec3(1.0,.7,.5), vec3(.6,.75,1.0), hash13(id+3.0));
    col += tint*(1.0-smoothstep(0.0,sz,d))*step(0.93,h)*tw*(1.5+4.0*h);
  }
  // sun glow bleeding in from the right edge
  float asp = uRes.x/uRes.y;
  float d = length(gP - vec2(asp*1.12, 0.0));
  float pulse = 0.55 + 0.05*sin(uTime*2.0);
  col += exp(-d*1.6)*vec3(1.0,.55,.20)*pulse + exp(-d*6.0)*vec3(1.0,.80,.50)*0.5;
  return col;
}

// ------------------------------------------------------------- tracer
vec3 render(vec3 ro, vec3 rd){
  vec3 col = background(rd);

  // Earth (ray marched tidal surface)
  float bb = dot(ro,rd), cc = dot(ro,ro) - 1.69, hE = bb*bb - cc;
  float tE = -1.0;
  if(hE > 0.0){
    float sq = sqrt(hE);
    float t = max(-bb-sq, 0.0), tEnd = -bb+sq;
    for(int i=0;i<110;i++){
      vec3 p = ro + rd*t;
      float f = length(p) - S(normalize(p));
      if(f < 0.0007){ tE = t; break; }
      t += f*0.55;
      if(t > tEnd) break;
    }
  }
  // Moon (analytic)
  vec3 oc = ro - MC;
  float bm = dot(oc,rd), cm = dot(oc,oc) - MOON_R*MOON_R, hm = bm*bm - cm;
  float tM = -1.0;
  if(hm > 0.0){ float tt = -bm - sqrt(hm); if(tt > 0.0) tM = tt; }

  bool useE = tE > 0.0 && (tM < 0.0 || tE < tM);
  bool useM = tM > 0.0 && !useE;
  float tS = useE ? tE : (useM ? tM : 1e5);
  if(useE) col = shadeEarth(ro + rd*tE, rd);
  else if(useM) col = shadeMoon(ro + rd*tM, rd);

  // orbit ring
  if(abs(rd.y) > 1e-4){
    float tr = -ro.y/rd.y;
    if(tr > 0.0 && tr < tS){
      vec3 hp = ro + rd*tr;
      float r = length(hp.xz);
      float ring = exp(-pow((r-MOON_ORBIT)/0.012, 2.0));
      float dash = smoothstep(0.3,0.5, fract(atan(hp.z,hp.x)*12.0/PI));
      col += vec3(.45,.6,1.0)*0.30*ring*dash;
    }
  }

  col += atmosphere(ro, rd, tS);
  return col;
}

vec3 aces(vec3 x){ return clamp((x*(2.51*x+0.03))/(x*(2.43*x+0.59)+0.14), 0.0, 1.0); }

void main(){
  MD  = vec3(cos(uMoon), 0.0, sin(uMoon));
  MC  = MD*MOON_ORBIT;
  ROT = rotY(uRot);

  vec3 ro = vec3(0.0, 6.2, 7.0);
  vec3 ww = normalize(-ro);
  vec3 uu = normalize(cross(ww, vec3(0.,1.,0.)));
  vec3 vv = cross(uu, ww);

  vec3 acc = vec3(0.0);
  for(int i=0;i<2;i++){
    for(int j=0;j<2;j++){
      vec2 off = (vec2(float(i),float(j)) - 0.5)*0.5;      // 2x2 supersampling
      vec2 p = (2.0*(gl_FragCoord.xy + off) - uRes)/uRes.y;
      gP = p;
      vec3 rd = normalize(ww + (p.x*uu + p.y*vv)*0.30);
      acc += render(ro, rd);
    }
  }
  acc *= 0.25;

  vec2 uv = gl_FragCoord.xy/uRes;
  vec3 col = aces(acc*1.1);
  col = pow(col, vec3(1.0/2.2));
  col *= 0.55 + 0.45*pow(16.0*uv.x*uv.y*(1.0-uv.x)*(1.0-uv.y), 0.2);   // vignette
  col += (hash13(vec3(gl_FragCoord.xy, uTime*60.0)) - 0.5)*0.012;        // film grain
  gl_FragColor = vec4(col, 1.0);
}
</script>

<script>
(function(){
  const canvas = document.getElementById('gl');
  const hud    = document.getElementById('hud');
  const errBox = document.getElementById('err');
  const slider = document.getElementById('moon');
  const degEl  = document.getElementById('deg');
  const status = document.getElementById('status');
  const auto   = document.getElementById('auto');
  const resSel = document.getElementById('res');

  function fail(msg){ errBox.style.display='block'; errBox.textContent=msg; hud.style.display='none'; }

  const gl = canvas.getContext('webgl', {antialias:false, powerPreference:'high-performance'});
  if(!gl){ fail('WebGL is not available in this browser.'); return; }

  function compile(type, id){
    const s = gl.createShader(type);
    gl.shaderSource(s, document.getElementById(id).textContent);
    gl.compileShader(s);
    if(!gl.getShaderParameter(s, gl.COMPILE_STATUS)){ fail('Shader error: ' + gl.getShaderInfoLog(s)); return null; }
    return s;
  }
  const vs = compile(gl.VERTEX_SHADER, 'vs'), fs = compile(gl.FRAGMENT_SHADER, 'fs');
  if(!vs || !fs) return;
  const prog = gl.createProgram();
  gl.attachShader(prog, vs); gl.attachShader(prog, fs); gl.linkProgram(prog);
  if(!gl.getProgramParameter(prog, gl.LINK_STATUS)){ fail('Link error: ' + gl.getProgramInfoLog(prog)); return; }
  gl.useProgram(prog);

  const buf = gl.createBuffer();
  gl.bindBuffer(gl.ARRAY_BUFFER, buf);
  gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([-1,-1, 1,-1, -1,1, 1,1]), gl.STATIC_DRAW);
  const loc = gl.getAttribLocation(prog, 'a');
  gl.enableVertexAttribArray(loc);
  gl.vertexAttribPointer(loc, 2, gl.FLOAT, false, 0, 0);

  const uRes = gl.getUniformLocation(prog,'uRes');
  const uTime = gl.getUniformLocation(prog,'uTime');
  const uMoon = gl.getUniformLocation(prog,'uMoon');
  const uRot  = gl.getUniformLocation(prog,'uRot');

  function setRes(){
    const h = parseInt(resSel.value, 10);
    canvas.height = h;
    canvas.width  = Math.round(h*16/9);
    gl.viewport(0, 0, canvas.width, canvas.height);
  }
  resSel.addEventListener('change', setRes);
  setRes();

  function updateUI(){
    const d = parseFloat(slider.value);
    degEl.textContent = Math.round(d);
    const m = d % 180;
    if(m < 15 || m > 165){ status.textContent = 'Spring Tide (Aligned: Maximum Range)'; status.style.color = '#ff7b72'; }
    else if(m > 75 && m < 105){ status.textContent = 'Neap Tide (Perpendicular: Minimum Range)'; status.style.color = '#3fb950'; }
    else { status.textContent = 'Intermediate Tide'; status.style.color = '#8b949e'; }
  }
  slider.addEventListener('input', updateUI);
  updateUI();

  canvas.addEventListener('webglcontextlost', function(e){
    e.preventDefault();
    fail('The GPU reset (too heavy for this device). Reload and pick a lower resolution.');
  });

  let last = performance.now(), rot = 0, frames = 0, fpsT = last, fps = 0;
  function frame(now){
    const dt = Math.min((now-last)/1000, 0.1); last = now;
    if(auto.checked){ slider.value = (parseFloat(slider.value) + dt*18) % 360; updateUI(); }
    rot += dt*0.25;

    gl.uniform2f(uRes, canvas.width, canvas.height);
    gl.uniform1f(uTime, now/1000);
    gl.uniform1f(uMoon, parseFloat(slider.value)*Math.PI/180);
    gl.uniform1f(uRot, rot);
    gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);

    frames++;
    if(now - fpsT > 500){
      fps = frames*1000/(now-fpsT); frames = 0; fpsT = now;
      const sppx = 4;
      hud.innerHTML = 'RAY TRACED &bull; ' + canvas.width + 'x' + canvas.height + '<br>' +
                      sppx + ' spp &bull; ' + fps.toFixed(1) + ' FPS';
    }
    requestAnimationFrame(frame);
  }
  requestAnimationFrame(frame);
})();
</script>
</body>
</html>
)rawliteral";

void setup() {
  Serial.begin(115200);
  Serial.println();

  // 1. Configure ESP32 as an Access Point
  Serial.println("Configuring Access Point...");
  WiFi.softAP(ssid, password);

  // 2. Fetch and print the IP address
  IPAddress IP = WiFi.softAPIP();
  Serial.print("AP IP address: ");
  Serial.println(IP);

  // 3. Define the web server route (send_P streams straight from flash, no heap copy)
  server.on("/", []() {
    server.send_P(200, "text/html", index_html);
  });

  // 4. Start the server
  server.begin();
  Serial.println("Web server started. Waiting for clients...");
}

void loop() {
  server.handleClient();
}