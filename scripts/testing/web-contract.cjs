// 用真实前端API模块访问隔离HTTP服务，不启动FramePipeline或模拟HTTP响应。
const assert=require('node:assert/strict');
const fs=require('node:fs');
const path=require('node:path');
const {createRequire}=require('node:module');
const {spawn}=require('node:child_process');
let stage='prepare';

function validateSchema(schema,value){
  if(schema.oneOf){
    assert.equal(schema.oneOf.filter(s=>{try{validateSchema(s,value);return true;}catch{return false;}}).length,1,'oneOf不唯一');
    return;
  }
  if(schema.type==='object'){
    assert.ok(value!==null && typeof value==='object' && !Array.isArray(value),'需要对象');
    for(const key of schema.required||[]) assert.ok(Object.hasOwn(value,key),'缺少必填字段');
    for(const [key,item] of Object.entries(value)){
      const child=schema.properties?.[key];
      if(!child) assert.notEqual(schema.additionalProperties,false,'存在未知字段');
      else validateSchema(child,item);
    }
  }else if(schema.type){
    assert.equal(typeof value,schema.type,'字段类型错误');
    if(typeof value==='number'){
      assert.ok(Number.isFinite(value),'非有限数值');
      if(schema.minimum!==undefined) assert.ok(value>=schema.minimum,'低于下界');
      if(schema.maximum!==undefined) assert.ok(value<=schema.maximum,'高于上界');
    }
  }
  if(schema.enum) assert.ok(schema.enum.includes(value),'枚举值错误');
}

function validateHealth(body,port){
  assert.equal(body.ok,true);
  assert.equal(body.data.name,'rk_wcfr');
  assert.equal(body.data.port,port);
  assert.ok(['ok','degraded','rejected','unknown'].includes(body.data.configStatus));
  assert.ok(['ok','degraded','starting'].includes(body.data.status));
  assert.ok(Array.isArray(body.data.availableFeatures));
}

async function live(root,web,exe,out){
  fs.mkdirSync(out,{recursive:false});
  const vite=await import(require('node:url').pathToFileURL(createRequire(path.join(web,'package.json')).resolve('vite')).href);
  // 使用项目现有Vite转换真实TS模块；middleware模式不开放第二个网络端口。
  const loader=await vite.createServer({configFile:false,root:path.join(root,'web'),
    cacheDir:path.join(out,'vite-cache'),server:{middlewareMode:true,watch:null,hmr:false},appType:'custom',
    optimizeDeps:{noDiscovery:true,include:[]},
    define:{'import.meta.env.VITE_API_BASE':JSON.stringify('')}});
  try {
  const schema=JSON.parse(fs.readFileSync(path.join(root,'docs/windows-web-spa/config.schema.json'),'utf8'));
  function example(s){
    if(s.oneOf)return example(s.oneOf[0]);
    if(s.enum)return s.enum[0];
    if(s.type==='object')return Object.fromEntries((s.required||[]).map(k=>[k,example(s.properties[k])]));
    if(s.type==='number')return s.minimum??0;
    return s.type==='boolean'?false:'';
  }
  const config=example(schema);
  config.log.logDir=path.join(out,'logs');
  config.recognition.databasePath=path.join(out,'gallery.yml');
  config.poster.enable=false; config.poster.postUrl='';
  fs.writeFileSync(path.join(out,'config.json'),JSON.stringify(config));
  const env={...process.env,RK_TEST_HTTP_CONTRACT:'1',RK_WCFR_CONFIG:path.join(out,'config.json')};
  // 仅子进程清除会覆盖本轮临时配置的变量，不读取或记录其原值。
  for(const key of ['RK_WCFR_HTTP_AUTH_TOKEN','RK_WCFR_HTTP_PORT','RK_WCFR_POST_URL','RK_WCFR_DNN_MODEL','RK_WCFR_DNN_CONFIG'])delete env[key];
  const child=spawn(exe,['--contract-host'],{cwd:out,env,windowsHide:true,stdio:'ignore'});
  const exited=new Promise((resolve,reject)=>{child.once('error',reject);child.once('exit',resolve);});
  let code;
  try{
    const ready=path.join(out,'ready.txt');
    for(let i=0;i<200 && !fs.existsSync(ready);i++){
      assert.equal(child.exitCode,null,'服务提前退出');
      await new Promise(r=>setTimeout(r,50));
    }
    assert.ok(fs.existsSync(ready),'服务未就绪');
    const port=Number(fs.readFileSync(ready,'utf8'));
    assert.ok(Number.isInteger(port)&&port>0&&port<=65535,'无效回环端口');
    const base=`http://127.0.0.1:${port}`;
    const nativeFetch=global.fetch;
    global.window={setTimeout,clearTimeout};
    global.localStorage={getItem:()=>null,setItem:()=>{},removeItem:()=>{}};
    global.fetch=(input,init)=>nativeFetch(new URL(input,base),init);
    const {defaultPrefs}=await loader.ssrLoadModule('/src/app/state/prefs.ts');
    const prefs={...defaultPrefs,logLevel:'silent',cacheStrategy:'no-cache'};
    const {getHealth}=await loader.ssrLoadModule('/src/app/api/health.ts');
    const {getServerSettings,putServerSettings}=await loader.ssrLoadModule('/src/app/api/settings.ts');
    stage='health';
    validateHealth(await getHealth(prefs),port);
    stage='settings-schema';
    const settings=await getServerSettings(prefs);
    assert.equal(settings.ok,true); validateSchema(schema,settings.data);
    stage='openapi';
    const api=await (await nativeFetch(base+'/api/v1/openapi')).json();
    for(const [route,method] of [['/api/v1/health','get'],['/api/v1/settings','get'],['/api/v1/settings','put']]){
      assert.ok(api.paths[route][method].responses['200'],'OpenAPI缺失操作');
    }
    stage='invalid-settings';
    await assert.rejects(()=>putServerSettings(prefs,{camera:{width:0}}),error=>error.httpStatus===400 && error.code==='invalid_request');
    stage='atomicity';
    const unchanged=await getServerSettings(prefs);
    assert.deepEqual(unchanged,settings,'拒绝非法更新后配置发生变化');
    assert.equal((await getHealth(prefs)).data.status,'starting','契约测试意外装配了相机管线');
    fs.writeFileSync(path.join(out,'junit.xml'),'<testsuite><testcase name="live_frontend_health"/><testcase name="live_frontend_settings_schema"/><testcase name="openapi_operations"/><testcase name="invalid_settings_error_and_atomicity"/></testsuite>');
    console.log('CONTRACT_PASS cases=4 camera_pipeline=absent');
  }finally{
    fs.writeFileSync(path.join(out,'stop.txt'),'stop');
    code=await Promise.race([exited,new Promise(resolve=>setTimeout(()=>resolve('timeout'),3000))]);
    if(code==='timeout')child.kill();
  }
  stage='cleanup';
  assert.equal(code,0,'自有服务未正常退出');
  } finally { await loader.close(); }
}

module.exports={validateSchema,validateHealth};
if(require.main===module){
  live(...process.argv.slice(2)).catch(()=>{console.error('CONTRACT_FAILED stage='+stage);process.exitCode=1;});
}
