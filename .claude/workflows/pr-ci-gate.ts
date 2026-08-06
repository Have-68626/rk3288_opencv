// PR 合并门禁工作流：CI 审计 → squash merge → 分支清理
// 用法（Workflow tool）：args = { pr: "<PR号>" }
export const meta = {
  name: 'pr-ci-gate',
  description: 'PR 合并门禁：CI 审计 → squash merge → 分支清理（squash and merge 规则）',
  phases: [
    { title: 'CI 审计', detail: 'gh pr checks 确认全部通过' },
    { title: '合并', detail: 'squash merge 到 master' },
    { title: '清理', detail: '删除已合并分支' },
  ],
}

const PR = (args && args.pr) || ''
if (!PR) {
  log('缺少参数 pr=<PR号>')
  return { status: 'error', reason: 'missing pr arg' }
}

phase('CI 审计')
const checks = await agent(
  `运行 gh pr checks ${PR} --json name,state,conclusion 并逐条判断。若所有 check 状态为成功/通过，返回 {pass:true}；否则 {pass:false, failing:["失败项"]}。只读，不改任何状态。`,
  { schema: {
      type: 'object',
      properties: { pass: { type: 'boolean' }, failing: { type: 'array', items: { type: 'string' } } },
      required: ['pass'],
    } }
)
if (!checks || !checks.pass) {
  log(`PR ${PR} CI 未通过，停止合并。`)
  return { status: 'blocked', failing: checks && checks.failing }
}

phase('合并')
const merged = await agent(
  `执行 gh pr merge ${PR} --squash --delete-branch。返回 {merged:boolean, error?:string}。`,
  { schema: {
      type: 'object',
      properties: { merged: { type: 'boolean' }, error: { type: 'string' } },
      required: ['merged'],
    } }
)
if (!merged || !merged.merged) {
  log(`PR ${PR} 合并失败：${merged && merged.error}`)
  return { status: 'merge-failed', error: merged && merged.error }
}

phase('清理')
await agent(`用 gh pr view ${PR} --json state,merged,mergedAt 确认 PR 已合并；返回 {state, merged}。`)

log(`PR ${PR} 已 squash merge 并清理分支`)
return { status: 'done', pr: PR }
