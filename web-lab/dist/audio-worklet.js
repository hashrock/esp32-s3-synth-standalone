import {Engine} from './engine.js';
class PianoProcessor extends AudioWorkletProcessor {
  constructor(){super();this.engine=new Engine(sampleRate);this.frames=0;this.port.onmessage=({data:d})=>{
    if(d.type==='configure'){this.engine.configure(d.bank);this.port.postMessage({type:'configured',revision:d.revision});}
    else if(d.type==='on')this.engine.on(d.note,d.velocity);
    else if(d.type==='off')this.engine.off(d.note);
    else if(d.type==='sustain')this.engine.sustain(!!d.on);
    else if(d.type==='panic')this.engine.panic();
  };}
  process(inputs,outputs){const output=outputs[0];if(!output?.length)return true;this.engine.render(output[0]);for(let ch=1;ch<output.length;ch++)output[ch].set(output[0]);
    this.frames+=output[0].length;
    if(this.frames>=sampleRate/12){this.frames=0;this.port.postMessage({type:'meter',voices:this.engine.voices.filter(v=>v.active).length,peak:this.engine.peak,gain:this.engine.limiter.gain,steals:this.engine.steals});this.engine.peak=0;}
    return true;
  }
}
registerProcessor('bark-piano',PianoProcessor);
