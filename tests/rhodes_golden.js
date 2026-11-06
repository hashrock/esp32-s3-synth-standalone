// Execute the actual supplied simulator, without browser or dependencies.
const fs = require('fs');
const vm = require('vm');
const html = fs.readFileSync('rhodes-physics-lab.html', 'utf8');
const start = html.indexOf('function simulate(');
const end = html.indexOf('\nfunction synth(', start);
const context = vm.createContext({});
vm.runInContext(`const SR=44100,OS=4,FS=SR*OS,DT=1/FS,SECONDS=4.2,TAU=Math.PI*2;
${html.slice(start,end)}
result=simulate(60,{v:78/127,k:.43,loss:.46,d:.35,gap:.66,offset:.55,nonlin:.22,cutoff:3486.08},false);`, context);
const data = fs.readFileSync((process.argv[2] || '/tmp') + '/rhodes-reference.f32');
const expected = context.result.out;
if (data.length !== expected.length*4) throw Error('length mismatch');
let max=0, sum=0;
for(let i=0;i<expected.length;i++) {
  const e=Math.abs(expected[i]-data.readFloatLE(i*4)); max=Math.max(max,e); sum+=e*e;
}
console.log(`Original JS vs C++ double: max=${max}, RMS=${Math.sqrt(sum/expected.length)}`);
if(max>1e-5) throw Error('golden mismatch');
