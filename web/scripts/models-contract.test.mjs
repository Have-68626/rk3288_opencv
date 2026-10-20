import { readFileSync } from 'node:fs'
import { describe, expect, it } from 'vitest'

const read = path => readFileSync(new URL(path, import.meta.url), 'utf8').replace(/\r\n/g, '\n')

describe('模型状态跨边界契约', () => {
  it('前端类型与 OpenAPI 描述的状态集合一致', () => {
    const type = read('../src/app/api/types.ts').match(/export interface ActiveModelInfo \{([\s\S]*?)\n\}/)[1]
    const typeLine = type.match(/status:([^\n]+)/)[1]
    const typeStates = [...typeLine.matchAll(/'([^']+)'/g)].map(m => m[1]).sort()
    const endpoint = read('../../docs/windows-web-spa/openapi.yaml')
      .split('  /api/v1/models:\n')[1].split('  /api/v1/models/reload:')[0]
    const docStates = [...endpoint.match(/`status`（([^）]+)）/)[1].matchAll(/`([^`]+)`/g)].map(m => m[1]).sort()
    expect(typeStates).toEqual(['disabled', 'failed', 'loaded', 'missing'])
    expect(docStates).toEqual(typeStates)
  })

  it('运行时实际生成的 disabled 状态有客户端契约', () => {
    expect(read('../../src/win/src/RuntimeBootstrap.cpp')).toMatch(/\.status\s*=\s*"disabled"/)
    expect(read('../src/app/api/types.ts').match(/status:([^\n]+)/)[1]).toContain("'disabled'")
  })
})
