// 缓存限单次只读快照，不能跨清理阶段复用过期文件状态。
const test=require('node:test');
const assert=require('node:assert/strict');
const {createSnapshotIO}=require('../../scripts/lib/snapshot-io');

test('同次目录读取复用，返回同一输入结果且不污染后续快照',async()=>{
  let calls=0;
  const io={readdir:async()=>{calls++;return ['固定输入'];},lstat:async()=>({size:7})};
  const first=createSnapshotIO(io);
  assert.deepEqual(await first.readdir('a',{withFileTypes:true}),['固定输入']);
  await first.readdir('a',{withFileTypes:true});
  assert.equal(calls,1);
  await createSnapshotIO(io).readdir('a',{withFileTypes:true});
  assert.equal(calls,2);
});
test('不同读取选项不得错误共享目录项',async()=>{
  let calls=0;
  const io={readdir:async()=>++calls,lstat:async()=>null};
  const cache=createSnapshotIO(io);
  assert.equal(await cache.readdir('a',{withFileTypes:true}),1);
  assert.equal(await cache.readdir('a'),2);
});
test('文件状态不缓存，后续判定可观察变化',async()=>{
  let calls=0;
  const cache=createSnapshotIO({readdir:async()=>[],lstat:async()=>({size:++calls})});
  const results=[await cache.lstat('a'),await cache.lstat('a')];
  assert.deepEqual(results,[{size:1},{size:2}]);assert.equal(calls,2);
});
