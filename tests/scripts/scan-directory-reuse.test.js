// 同次只读扫描的多个glob应共享目录读取，清理阶段不使用该缓存。
const test=require('node:test');
const assert=require('node:assert/strict');
const fs=require('node:fs');
const os=require('node:os');
const path=require('node:path');
const {execFileSync}=require('node:child_process');

test('多个glob仅为每目录再读取一次，不改变候选结果',()=>{
  const area=fs.mkdtempSync(path.join(os.tmpdir(),'scan-reuse-'));
  const root=path.join(area,'repo');
  try{
    fs.mkdirSync(path.join(root,'.trae/specs/clean-repo-junk'),{recursive:true});
    fs.writeFileSync(path.join(root,'.trae/specs/clean-repo-junk/spec.md'),'# fixture');
    fs.mkdirSync(path.join(root,'data'));
    for(const name of ['alpha.tmp','beta.tmp'])fs.writeFileSync(path.join(root,'data',name),'fixed');
    const rules=path.join(area,'rules.json');
    fs.writeFileSync(rules,JSON.stringify({targets:['alpha.tmp','beta.tmp'].map(pattern=>({id:pattern,kind:'glob',pattern:'**/'+pattern}))}));
    const counter=path.join(area,'counter.cjs');
    const countsFile=path.join(area,'counts.json');
    fs.writeFileSync(counter,`const fs=require('fs');const fsp=require('fs/promises');const counts={};const original=fsp.readdir;fsp.readdir=function(file,...args){counts[String(file)]=(counts[String(file)]||0)+1;return original.call(this,file,...args);};process.on('exit',()=>fs.writeFileSync(process.env.RK_SCAN_COUNTS,JSON.stringify(counts)));`);
    const output=path.join(area,'reports');
    execFileSync(process.execPath,['--require',counter,path.resolve(__dirname,'../../scripts/clean-repo-junk.js'),'scan','--ci','--root',root,'--config',rules,'--output-dir',output],{env:{...process.env,RK_SCAN_COUNTS:countsFile},encoding:'utf8'});
    const counts=JSON.parse(fs.readFileSync(countsFile,'utf8'));
    assert.equal(counts[root],2,'盘点与规则匹配各自仅读一次根目录');
    assert.equal(counts[path.join(root,'data')],2);
    const name=fs.readdirSync(output).find(name=>name.startsWith('scan-report-')&&name.endsWith('.json'));
    const report=JSON.parse(fs.readFileSync(path.join(output,name),'utf8'));
    assert.deepEqual(report.candidates.map(item=>item.relPath).sort(),['data/alpha.tmp','data/beta.tmp']);
  }finally{fs.rmSync(area,{recursive:true,force:true});}
});
