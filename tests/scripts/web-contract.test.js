// 契约判读必须拒绝字段、类型和错误信封变异。
const test=require('node:test');
const assert=require('node:assert/strict');
const {validateSchema, validateHealth}=require('../../scripts/testing/web-contract.cjs');

test('配置响应遵循必填、类型、范围和未知字段约束',()=>{
  const schema={type:'object',required:['port'],additionalProperties:false,properties:{port:{type:'number',minimum:1,maximum:65535}}};
  validateSchema(schema,{port:8000});
  for(const value of [{},{port:true},{port:0},{port:8000,extra:1}]){
    assert.throws(()=>validateSchema(schema,value));
  }
});
test('健康响应缺诊断和假成功都不能通过',()=>{
  const good={ok:true,data:{name:'rk_wcfr',port:1234,configStatus:'ok',status:'starting',availableFeatures:[]}};
  validateHealth(good,1234);
  for(const value of [{...good,ok:false},{ok:true,data:{...good.data,port:1235}},{ok:true,data:{...good.data,configStatus:undefined}}]){
    assert.throws(()=>validateHealth(value,1234));
  }
});
test('oneOf必须唯一匹配',()=>{
  validateSchema({oneOf:[{type:'string'},{type:'number'}]},'public');
  assert.throws(()=>validateSchema({oneOf:[{type:'number'},{type:'number'}]},1));
});
