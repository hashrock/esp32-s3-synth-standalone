// Browser port of main/dsp/rhodes.h and limiter.h. Double precision in JS;
// the firmware uses float. All coefficients are built outside the render loop.
export const PRESETS = {
  bark: {"name":"Bark","stiffness":0.39,"damping":0.01,"cutoff":10000,"drive":0.25,"release_seconds":0.041,"pickup_mix":0.6,"pickup_gap":0.00132,"pickup_offset":-0.00011,"volume":1,"velocity_timbre":true,"saturate_output":false,"tremolo":false},
  rhodes: {name:'Rhodes',stiffness:.43,damping:.35,cutoff:3486.08,drive:.1625,release_seconds:.07,pickup_mix:0,pickup_gap:.001,pickup_offset:.00022,velocity_timbre:false,saturate_output:true,volume:1,tremolo:false},
  velvet: {name:'Velvet Trem',stiffness:.28,damping:.22,cutoff:1900,drive:.145,release_seconds:.10,pickup_mix:0,pickup_gap:.001,pickup_offset:.00022,velocity_timbre:false,saturate_output:true,volume:1.35,tremolo:true}
};
export const LIMITS={stiffness:[0,1],damping:[0,1],cutoff:[400,10000],drive:[.02,.5],release_seconds:[.015,.3],pickup_mix:[0,1],pickup_gap:[.0007,.003],pickup_offset:[-.001,.001],volume:[.1,2]};
export function validateSettings(input) {
  if(!input || typeof input!=='object') throw Error('設定の形式が正しくありません。');
  const s={name: typeof input.name==='string'?input.name.slice(0,48):'Custom'};
  for(const [key,[lo,hi]] of Object.entries(LIMITS)) {
    const v=input[key];
    if(typeof v!=='number'||!Number.isFinite(v)||v<lo||v>hi) throw Error(`${key} が範囲外です。`);
    s[key]=v;
  }
  for(const key of ['velocity_timbre','saturate_output','tremolo']) {
    if(typeof input[key]!=='boolean') throw Error(`${key} の形式が正しくありません。`);
    s[key]=input[key];
  }
  return s;
}
export function encodePatch(settings) {return JSON.stringify({format:'epiano-tone',version:1,settings:validateSettings(settings)},null,2);}
export function decodePatch(text) {
  if(text.length>32768) throw Error('設定ファイルが大きすぎます。');
  const p=JSON.parse(text);
  if(p.format!=='epiano-tone'||p.version!==1) throw Error('対応していない設定ファイルです。');
  return validateSettings(p.settings);
}
export function cppSettings(settings) {
  const s=validateSettings(settings);
  return `#pragma once\n#include "dsp/rhodes.h"\n\nnamespace tone_lab {\n// stiffness, damping, cutoff, linear level, release, pickup mix, gap, offset,\n// velocity timbre, saturation\ninline constexpr rhodes::Settings kTone{\n    ${['stiffness','damping','cutoff','drive','release_seconds','pickup_mix','pickup_gap','pickup_offset','velocity_timbre','saturate_output'].map(k=>String(s[k])).join(', ')}\n};\ninline constexpr float kVolume = ${s.volume.toFixed(6)}f;\ninline constexpr bool kTremolo = ${s.tremolo};\n} // namespace tone_lab\n`;
}
const TAU=2*Math.PI;
export function makeBank(input,sr=48000) {
  const s=validateSettings(input), b={s,sr,maxFrames:sr*16,release:Math.exp(-1/(s.release_seconds*sr)),dcAlpha:1-Math.pow(1-.0006,44100/sr),stiffness:2.5e7*Math.exp((s.stiffness-.5)*2.3),notes:[],tones:[],sat:new Float64Array(1025),pickup:new Float64Array(1025)};
  for(let v=0;v<128;v++) {
    const strength=s.velocity_timbre?(v/127)**2:1;
    b.tones.push({alpha:1-Math.exp(-TAU*s.cutoff*(.26+.74*strength)/sr),level:s.drive*(s.saturate_output?.45+.55*strength:1),positionScale:64000*(.12+.88*strength)});
  }
  const nt=-s.pickup_offset/s.pickup_gap,nominal=2*nt/(1+nt*nt)**2;
  for(let i=0;i<=1024;i++) {
    b.sat[i]=Math.tanh(i*8/1024);
    const t=((i-512)*.016/1024-s.pickup_offset)/s.pickup_gap;
    const ratio=nominal===0?1:(2*t/(1+t*t)**2)/nominal;
    b.pickup[i]=1-s.pickup_mix+s.pickup_mix*Math.max(-3,Math.min(3,ratio));
  }
  const ratios=[1,6.267,17.548,34.386,56.84],shapes=[1,-.43,.2,-.1,.05];
  for(let n=0;n<128;n++) {
    const f=440*2**((n-69)/12),os=f*6.267<=Math.min(11000,sr*.4)?2:4,dt=1/(sr*os),modes=[];
    for(let j=0;j<5;j++) {
      const hz=f*ratios[j];if(hz>11000||hz>sr*.4)break;
      const w=TAU*hz,w2=w*w,gamma=(.42+2*s.damping)*(j===0?1:5**j*.25),force=shapes[j]**2/(.004*(1+.85*j));
      const wd=Math.sqrt(w2-gamma*gamma),e=Math.exp(-gamma/sr),sn=Math.sin(wd/sr)/wd,c=Math.cos(wd/sr);
      modes.push({invW:1/w,stepV:1-2*gamma*dt,stepQ:-w2*dt,stepF:force*dt,a:e*(c+gamma*sn),b:e*sn,c:-e*w2*sn,d:e*(c-gamma*sn)});
    }
    b.notes.push({os,dt,modes});
  }
  return b;
}
function lookup(table,index) {
  if(index<=0)return table[0];if(index>=1024)return table[1024];
  const i=Math.floor(index);return table[i]+(index-i)*(table[i+1]-table[i]);
}
export class Voice {
  constructor(){this.q=new Float64Array(5);this.v=new Float64Array(5);this.active=false;this.tail=0;this.last=0;this.serial=0;this.gate=false;this.sustained=false;}
  on(bank,note,velocity,serial=0) {
    const previous=this.active?this.last*this.mixGain:0;
    this.bank=bank;this.note=note;this.mode=bank.notes[note];this.tone=bank.tones[velocity];
    this.mixGain=2*bank.s.volume;this.tail=previous/this.mixGain;this.serial=serial;
    this.q.fill(0);this.v.fill(0);this.hx=-.0007;this.hv=.35+2.8*velocity/127;
    this.lp=this.dc=this.tipX=this.tipV=this.age=0;this.gain=1;this.free=false;this.released=false;
    this.active=velocity>0&&this.mode.modes.length>0;this.gate=true;this.sustained=false;this.last=0;
  }
  off(){this.released=true;this.gate=false;this.sustained=false;}
  process() {
    if(!this.active)return 0;
    const b=this.bank,modes=this.mode.modes;let velocity=0,position=0;
    if(this.free) {
      for(let j=0;j<modes.length;j++) {
        const m=modes[j],q=this.q[j],v=this.v[j];
        this.q[j]=m.a*q+m.b*v;this.v[j]=m.c*q+m.d*v;
        velocity+=this.v[j];position+=this.q[j];
      }
    } else {
      let x=this.tipX,v=this.tipV;const dt=this.mode.dt;
      for(let sub=0;sub<this.mode.os;sub++) {
        const compression=this.hx-x,damping=1+1.482*(this.hv-v);
        const force=compression>0&&damping>0?Math.min(150,b.stiffness*compression*Math.sqrt(compression)*damping):0;
        this.hv-=force*100*dt;this.hx+=this.hv*dt;x=v=0;
        for(let j=0;j<modes.length;j++) {
          const m=modes[j];this.v[j]=m.stepV*this.v[j]+m.stepQ*this.q[j]+m.stepF*force;
          this.q[j]+=this.v[j]*dt;x+=this.q[j];v+=this.v[j];
        }
      }
      this.tipX=position=x;this.tipV=velocity=v;
      if(this.hv<0&&this.hx<0) {
        let bound=0;for(let j=0;j<modes.length;j++)bound+=Math.abs(this.q[j])+Math.abs(this.v[j])*modes[j].invW;
        if(this.hx<-1.01*bound)this.free=true;
      }
    }
    if(b.s.pickup_mix)velocity*=lookup(b.pickup,position*this.tone.positionScale+512);
    this.lp+=this.tone.alpha*(velocity/(TAU*220*.0004)-this.lp);
    this.dc+=b.dcAlpha*(this.lp-this.dc);
    if(this.released)this.gain*=b.release;
    const linear=(this.lp-this.dc)*this.tone.level;
    const signal=b.s.saturate_output?Math.sign(linear)*lookup(b.sat,Math.abs(linear)*128):linear;
    const result=signal*.62*this.gain;
    if(this.gain<1e-5||++this.age>=b.maxFrames)this.active=false;
    return result;
  }
}
export class Limiter {
  constructor(sr){this.pending=new Float64Array(64);this.pendingPeak=0;this.gain=1;this.releaseStep=1-Math.exp(-64/(sr*.08));}
  reset(){this.pending.fill(0);this.pendingPeak=0;this.gain=1;}
  process(samples,peak) {
    const safe=this.pendingPeak>.95?.95/this.pendingPeak:1,next=peak>.95?.95/peak:1;
    const end=Math.min(safe,next,this.gain+(1-this.gain)*this.releaseStep),step=(end-this.gain)/64;
    for(let i=0;i<64;i++){const input=samples[i];samples[i]=this.pending[i]*Math.min(safe,this.gain+(i+1)*step);this.pending[i]=input;}
    this.gain=end;this.pendingPeak=peak;
  }
}
export class Engine {
  constructor(sr=48000){this.sr=sr;this.bank=makeBank(PRESETS.bark,sr);this.voices=Array.from({length:8},()=>new Voice());this.mix=new Float64Array(64);this.trem=new Float64Array(64);this.limiter=new Limiter(sr);this.read=64;this.serial=0;this.pedal=false;this.phase=0;this.inc=Math.trunc(4.2/sr*4294967296);this.peak=0;this.steals=0;}
  configure(bank){this.bank=bank;}
  on(note,velocity) {
    if(!Number.isInteger(note)||note<0||note>127)return;
    velocity=Math.max(0,Math.min(127,Math.round(velocity)));
    if(!velocity){this.off(note);return;}
    let v=this.voices.find(v=>v.active&&v.note===note)||this.voices.find(v=>!v.active);
    if(!v) {this.steals++;const releasing=this.voices.filter(v=>!v.gate);v=releasing.length?releasing.reduce((a,b)=>Math.abs(a.last)<Math.abs(b.last)?a:b):this.voices.reduce((a,b)=>a.serial<b.serial?a:b);}
    v.on(this.bank,note,velocity,++this.serial);
  }
  off(note){for(const v of this.voices)if(v.active&&v.gate&&v.note===note){if(this.pedal){v.gate=false;v.sustained=true;}else v.off();}}
  sustain(on){this.pedal=on;if(!on)for(const v of this.voices)if(v.sustained)v.off();}
  panic(){for(const v of this.voices){v.active=false;v.tail=v.last=0;v.gate=v.sustained=false;}this.pedal=false;this.mix.fill(0);this.limiter.reset();this.read=64;this.peak=0;}
  block() {
    this.mix.fill(0);this.trem.fill(0);let hasTrem=false;
    for(const v of this.voices) {
      if(!v.active)continue;
      const target=v.bank.s.tremolo?this.trem:this.mix;hasTrem||=v.bank.s.tremolo;
      for(let i=0;i<64;i++){v.last=v.process()+v.tail;v.tail*=.9862071;target[i]+=v.last*v.mixGain;}
    }
    const next=(this.phase+this.inc*64)>>>0;
    if(hasTrem){let gain=.7+.3*Math.sin(TAU*this.phase/4294967296);const step=(.7+.3*Math.sin(TAU*next/4294967296)-gain)/64;for(let i=0;i<64;i++){this.mix[i]+=this.trem[i]*gain;gain+=step;}}
    this.phase=next;let peak=0;
    for(let i=0;i<64;i++){this.mix[i]*=.12;peak=Math.max(peak,Math.abs(this.mix[i]));}
    this.peak=Math.max(this.peak,peak);this.limiter.process(this.mix,peak);
  }
  render(out){for(let i=0;i<out.length;i++){if(this.read===64){this.block();this.read=0;}out[i]=this.mix[this.read++];}}
}
