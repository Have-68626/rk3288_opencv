const test = require('node:test')
const assert = require('node:assert/strict')
const fs = require('node:fs')
const vm = require('node:vm')

const workflow = fs.readFileSync(process.env.REVIEW_CI_WORKFLOW || '.github/workflows/ci.yml', 'utf8').replace(/\r\n/g, '\n')
const gate = workflow.match(/cd web && node -e "\n([\s\S]*?)\n\s*"\n/)[1]
function runGate(input, auditFailed = false) {
  const output = []
  const stdout = typeof input === 'string' ? input : JSON.stringify(input)
  try {
    vm.runInNewContext(gate, {
      require: () => ({ execSync: () => {
        if (auditFailed) throw Object.assign(new Error('审计发现漏洞'), { stdout })
        return stdout
      } }),
      process: { exit: code => { throw Object.assign(new Error('门禁退出'), { exitCode: code }) } },
      console: { log: text => output.push(text), error: text => output.push(text) },
    }, { timeout: 1000 })
    return { code: 0, output }
  } catch (error) {
    return { code: error.exitCode || 2, output }
  }
}

test('无漏洞的已支持 schema 可以通过', () => {
  assert.equal(runGate({ advisories: {} }).code, 0)
})
test('非豁免 high 阻断，沿用 pnpm 非零退出的 stdout', () => {
  assert.notEqual(runGate({ advisories: { high: { severity: 'high', title: '验证漏洞' } } }, true).code, 0)
})
test('critical 必须阻断', () => {
  assert.notEqual(runGate({ advisories: { critical: { severity: 'critical', title: '验证漏洞' } } }, true).code, 0)
})
test('已批准的 extract-zip high 例外保持可用', () => {
  assert.equal(runGate({ advisories: { exempt: { severity: 'high', url: 'GHSA-jmr9-qjv8-65gv' } } }, true).code, 0)
})
test('critical 不继承 high 级例外', () => {
  assert.notEqual(runGate({ advisories: { critical: { severity: 'critical', url: 'GHSA-jmr9-qjv8-65gv' } } }, true).code, 0)
})
for (const [name, data] of [
  ['新 vulnerabilities schema', { vulnerabilities: { high: { severity: 'high' } } }],
  ['缺少 advisories', {}], ['null advisories', { advisories: null }],
  ['数组 advisories', { advisories: [] }], ['字符串 advisories', { advisories: 'unknown' }],
  ['损坏 JSON', '{'],
]) test(`未支持或无效输出不得假绿：${name}`, () => {
  const result = runGate(data)
  assert.notEqual(result.code, 0)
  assert.equal(result.output.some(line => line.startsWith('Audit OK')), false)
})
