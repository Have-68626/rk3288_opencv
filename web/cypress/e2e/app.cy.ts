// 本文件仅验证浏览器 UI 的 mock 冒烟，不承担真实 Windows 后端联调。
import * as settingsFixture from '../fixtures/settings.json'

function serveSettings(doc: Record<string, unknown> = Cypress._.cloneDeep(settingsFixture)) {
  cy.intercept('GET', '/api/v1/settings', (request) => request.reply({ body: { ok: true, data: doc } })).as('settings')
  cy.intercept('PUT', '/api/v1/settings', (request) => {
    doc = Cypress._.merge({}, doc, request.body)
    request.reply({ body: { ok: true, data: doc } })
  }).as('saveSettings')
}

function visitSettings(ready = true) {
  cy.visit('/', {
    onBeforeLoad(win) {
      win.localStorage.setItem('rk_wcfr_web_prefs_v1', JSON.stringify({ startPage: 'settings', cacheStrategy: 'no-cache' }))
    },
  })
  cy.contains('[role="tab"]', '后端（API）').click()
  cy.contains('button', '保存到后端').should(ready ? 'be.enabled' : 'be.disabled')
  expandPanel('相机（Camera）')
}

function expandPanel(label: string) {
  cy.contains('.ant-collapse-header', label).click()
}

describe('RK3288 本地控制台（mock 冒烟）', () => {
  beforeEach(() => {
    // 未单独声明的 API 也明确隔离，避免 CI 意外依赖本机 8080 后端。
    cy.intercept('/api/**', {
      statusCode: 503,
      body: { ok: false, error: { code: 'unavailable', message: 'mock 未提供此端点' } },
    })
    cy.intercept('GET', '/api/v1/health', {
      body: { ok: true, data: { name: 'rk_wcfr', port: 8080, configStatus: 'ok', status: 'ok', availableFeatures: [] } },
    })
    cy.intercept('GET', '/api/v1/models', {
      body: {
        ok: true,
        data: {
          supportedModels: [], activeModels: [],
          summary: { totalSupported: 0, totalConfigured: 0, totalLoaded: 0, totalFailed: 0, totalMissing: 0 },
        },
      },
    })
  })

  it('本地偏好实时写入，并在刷新后保留', () => {
    cy.intercept('GET', '/api/v1/settings', {
      statusCode: 503,
      body: { ok: false, error: { code: 'unavailable', message: 'offline' } },
    }).as('getSettingsOffline')

    cy.visit('/', {
      onBeforeLoad(win) {
        win.localStorage.setItem(
          'rk_wcfr_web_prefs_v1',
          JSON.stringify({
            theme: 'light',
            language: 'zh-CN',
            startPage: 'home',
            apiTimeoutMs: 8000,
            logLevel: 'info',
            cacheStrategy: 'no-cache',
          }),
        )
      },
    })

    cy.contains('RK3288 本地控制台')
    cy.wait('@getSettingsOffline')
    cy.contains('继续进入').click()
    cy.contains('概览')

    cy.get('.ant-menu').contains('设置').click()
    cy.contains('基础（主题 / 语言 / 启动页）')
    cy.get('#theme').closest('.ant-select').find('.ant-select-selector').click()
    cy.get('.ant-select-dropdown').contains('深色').click()
    cy.window().then((win) => {
      const prefs = JSON.parse(win.localStorage.getItem('rk_wcfr_web_prefs_v1') || '{}')
      expect(prefs.theme).to.equal('dark')
    })
    cy.reload()
    cy.get('#theme').closest('.ant-select').should('contain', '深色')
  })

  it('后端 settings 请求失败时能展示错误提示', () => {
    cy.intercept('GET', '/api/v1/settings', {
      statusCode: 500,
      body: { ok: false, error: { code: 'internal_error', message: 'boom' } },
    }).as('getSettings')

    cy.visit('/#/home')
    // 直接进入概览不会执行启动页的初始化请求，需要先触发刷新。
    cy.contains('button', '刷新后端设置').first().click()
    cy.wait('@getSettings')
    cy.contains('后端 /api/v1/settings 不可用')
  })

  it('完整后端设置只提交编辑字段，并能恢复未保存编辑', () => {
    serveSettings()
    visitSettings()
    cy.contains('button', '保存到后端').click()
    cy.contains('没有检测到变更')
    cy.get('#camera_width').clear().type('800').blur()
    cy.contains('button', '保存到后端').click()
    cy.wait('@saveSettings').its('request.body').should('deep.equal', { camera: { width: 800 } })
    cy.contains('保存成功')
    cy.get('#camera_width').clear().type('960').blur()
    cy.contains('button', '恢复').click()
    cy.get('.ant-popconfirm').contains('button', /确\s*认/).click()
    cy.get('#camera_width').should('have.value', '800')
  })

  it('脱敏上报地址不会回写占位符，允许显式清空并拒绝无效地址', () => {
    serveSettings()
    visitSettings()
    expandPanel('上报（Poster）')
    cy.get('#poster_postUrl').should('have.value', '***').clear().type('invalid')
    cy.contains('button', '保存到后端').click()
    cy.contains('POST URL 需填写')
    cy.get('#poster_postUrl').clear().type('**')
    cy.contains('button', '保存到后端').click()
    cy.contains('POST URL 需填写')
    cy.get('#poster_postUrl').clear().type('***')
    cy.contains('button', '保存到后端').click()
    cy.contains('没有检测到变更')
    cy.get('#poster_postUrl').clear()
    cy.contains('button', '保存到后端').click()
    cy.wait('@saveSettings').its('request.body').should('deep.equal', { poster: { postUrl: '' } })
    cy.get('#poster_postUrl').type('https://receiver.invalid/faces')
    cy.contains('button', '保存到后端').click()
    cy.wait('@saveSettings').its('request.body').should('deep.equal', { poster: { postUrl: 'https://receiver.invalid/faces' } })
  })

  it('保存失败显示字段原因并结束加载，重试可成功', () => {
    serveSettings()
    visitSettings()
    cy.intercept('PUT', '/api/v1/settings', { statusCode: 400, body: { ok: false, error: { code: 'invalid', message: '拒绝写入', details: ['camera.width 不允许'] } } }).as('badSave')
    cy.get('#camera_width').clear().type('800').blur()
    cy.contains('button', '保存到后端').click()
    cy.wait('@badSave')
    cy.contains('拒绝写入：camera.width 不允许')
    cy.contains('正在应用设置并重启引擎').should('not.exist')
    cy.intercept('PUT', '/api/v1/settings', { body: { ok: true, data: { ...settingsFixture, camera: { ...settingsFixture.camera, width: 800 } } } }).as('retrySave')
    cy.contains('button', '保存到后端').click()
    cy.wait('@retrySave')
    cy.get('#camera_width').should('have.value', '800')
  })

  it('启动失败后可重试连接，并按设置进入启动页', () => {
    cy.intercept('GET', '/api/v1/settings', { body: { ok: false, error: { code: 'unavailable', message: '暂未就绪' } } })
    cy.visit('/')
    cy.contains('后端设置拉取失败')
    serveSettings()
    cy.contains('button', '重试').click()
    cy.contains('RK3288 控制台')
    cy.contains('button', '刷新后端设置').first().click()
    cy.contains('操作成功')
  })

  it('模型状态区分加载、失败、禁用与缺失，重载失败后可重试', () => {
    serveSettings()
    const statuses = ['loaded', 'failed', 'disabled', 'missing']
    const models = { activeModels: statuses.map((status, index) => ({ id: `model-${index}`, displayName: `模型-${status}`, taskType: 'recognition', backend: 'ncnn', status, isInUse: index === 0, configuredPath: 'fixtures/model', resolvedPath: 'fixtures/model', hash: 'fixture-model-hash', modelVersion: 'fixture', lastError: index === 1 ? '合成加载失败' : '' })), supportedModels: [], summary: { totalSupported: 4, totalConfigured: 4, totalLoaded: 1, totalFailed: 1, totalMissing: 1 } }
    cy.intercept('GET', '/api/v1/models', { body: { ok: true, data: models } })
    visitSettings()
    expandPanel('模型状态 (Model Status)')
    cy.contains('已禁用 1')
    cy.contains('缺失 1')
    cy.contains('合成加载失败')
    cy.intercept('POST', '/api/v1/models/reload', { statusCode: 500, body: { ok: false, error: { code: 'reload_failed', message: '重载被拒绝' } } })
    cy.contains('.ant-card-head-title', '模型-loaded').closest('.ant-card').contains('button', /重\s*载/).click()
    cy.contains('重载被拒绝')
    cy.intercept('POST', '/api/v1/models/reload', { body: { ok: false, error: { code: 'reload_failed', message: '合成重载信封失败' } } })
    cy.contains('.ant-card-head-title', '模型-loaded').closest('.ant-card').contains('button', /重\s*载/).click()
    cy.contains('合成重载信封失败')
    cy.intercept('POST', '/api/v1/models/reload', { body: { ok: true, data: { ok: true, status: 'loaded' } } })
    cy.contains('.ant-card-head-title', '模型-loaded').closest('.ant-card').contains('button', /重\s*载/).click()
    cy.contains('已请求重载 模型-loaded')
  })

  it('运行时降级重试报告装配失败，并在恢复后移除横幅', () => {
    serveSettings()
    cy.intercept('GET', '/api/v1/health', { body: { ok: true, data: { name: 'fixture', port: 8080, configStatus: 'ok', status: 'degraded', availableFeatures: [], reasonCode: 'missing_model', warning: '合成模型尚未就绪', degradedSince: '2026-10-16' } } })
    cy.intercept('GET', '/api/v1/models', { body: { ok: true, data: { activeModels: [{ id: 'fixture-model' }], summary: {}, supportedModels: [] } } })
    cy.intercept('POST', '/api/v1/models/reload', { body: { ok: true, data: { ok: false, warning: '合成装配失败' } } })
    cy.visit('/')
    cy.contains('引擎处于降级运行模式')
    cy.contains('button', '重试').click()
    cy.contains('重试后仍处于降级：合成装配失败')
    cy.intercept('POST', '/api/v1/models/reload', { body: { ok: true, data: { ok: true } } })
    cy.intercept('GET', '/api/v1/health', { body: { ok: true, data: { status: 'ok', availableFeatures: [] } } })
    cy.contains('button', '重试').click()
    cy.contains('引擎处于降级运行模式').should('not.exist')
  })

  it('本地鉴权保存、清除与空值保存控制后续请求头', () => {
    serveSettings()
    cy.visit('/#/settings', { onBeforeLoad(win) { win.localStorage.setItem('rk_wcfr_web_prefs_v1', JSON.stringify({ cacheStrategy: 'no-cache' })) } })
    cy.contains('button', '保存 Token').click()
    cy.contains('Token 已清除')
    // 合成占位值只用于被 intercept 隔离的请求，禁止使用任何真实凭据。
    cy.get('input[placeholder="留空表示不启用鉴权"]').type('fixture-only-value', { log: false })
    cy.contains('button', '保存 Token').click()
    cy.contains('已启用 Bearer 鉴权')
    cy.contains('button', '刷新后端设置').first().click()
    cy.wait('@settings').its('request.headers').should('have.property', 'authorization')
    cy.contains('button', '清除 Token').click()
    cy.contains('button', '清除 Token').should('be.disabled')
    cy.contains('button', '刷新后端设置').first().click()
    cy.wait('@settings').its('request.headers').should('not.have.property', 'authorization')
  })

  it('本地高级策略和语言立即生效，并持久化合法值', () => {
    cy.visit('/#/settings')
    const choose = (id: string, text: string) => {
      cy.get(`#${id}`).closest('.ant-select').find('.ant-select-selector').click()
      cy.get('.ant-select-dropdown:visible').contains('.ant-select-item-option-content', text).click()
    }
    choose('language', 'English')
    choose('startPage', '设置')
    choose('theme', '浅色')
    expandPanel('高级（API 超时 / 日志级别 / 缓存策略）')
    cy.get('#apiTimeoutMs').clear().type('1200').blur()
    choose('logLevel', '调试')
    choose('cacheStrategy', '本地 5min')
    cy.window().then((win) => {
      const prefs = JSON.parse(win.localStorage.getItem('rk_wcfr_web_prefs_v1') || '{}')
      expect(prefs).to.include({ language: 'en-US', startPage: 'settings', theme: 'light', apiTimeoutMs: 1200, logLevel: 'debug', cacheStrategy: 'local-5m' })
    })
  })

  it('空段文档使用显示默认值，缺段文档禁用保存', () => {
    const sparse = Object.fromEntries(Object.keys(settingsFixture).filter((key) => key !== 'schemaVersion').map((key) => [key, {}]))
    serveSettings({ schemaVersion: 1, ...sparse })
    visitSettings()
    cy.get('#camera_width').should('have.value', '640')
    cy.get('#http_enable').click()
    cy.contains('button', '保存到后端').click()
    cy.wait('@saveSettings').its('request.body').should('deep.equal', { http: { enable: true } })
    serveSettings({ schemaVersion: 1, _error: 'key_error' })
    cy.contains('button', '重新拉取').click()
    cy.contains('button', '保存到后端').should('be.disabled')
  })

  it('轮换动作失败提示后可再次确认成功，所有动作均由 mock 接收', () => {
    serveSettings()
    visitSettings()
    cy.intercept('POST', '/api/v1/actions/crypto/rotate', { statusCode: 500, body: { ok: false, error: { code: 'fixture_failure', message: '合成轮换失败' } } }).as('rotateFailure')
    cy.contains('button', '轮换密钥').click()
    cy.get('.ant-popconfirm').contains('button', '确认轮换').click()
    cy.wait('@rotateFailure')
    cy.contains('合成轮换失败')
    cy.intercept('POST', '/api/v1/actions/crypto/rotate', { body: { ok: true } }).as('rotateSuccess')
    cy.contains('button', '轮换密钥').click()
    cy.get('.ant-popconfirm').contains('button', '确认轮换').click()
    cy.wait('@rotateSuccess')
    cy.contains('已触发密钥轮换')
  })

  for (const scenario of [
    { name: '缺少 envelope', response: { body: {} }, expected: 'API 响应缺少统一 envelope 结构' },
    { name: '空 HTTP 错误', response: { statusCode: 503, body: '' }, expected: 'HTTP 503' },
    { name: '解析失败', response: { body: 'not-json', headers: { 'content-type': 'text/plain' } }, expected: '后端设置拉取失败' },
    { name: '超时', response: { delay: 900, body: { ok: true, data: settingsFixture } }, expected: '请求超时' },
  ]) {
    it(`启动请求${scenario.name}时保留可重试界面`, () => {
      cy.intercept('GET', '/api/v1/settings', scenario.response)
      cy.visit('/', { onBeforeLoad(win) { win.localStorage.setItem('rk_wcfr_web_prefs_v1', JSON.stringify({ apiTimeoutMs: 500, cacheStrategy: 'no-cache' })) } })
      cy.contains(scenario.expected)
      cy.contains('button', '重试').should('be.enabled')
    })
  }

  it('内存缓存避免重复请求，过期后重新请求服务端', () => {
    serveSettings()
    cy.clock(Date.now(), ['Date'])
    cy.visit('/')
    cy.contains('RK3288 控制台')
    cy.contains('button', '刷新后端设置').first().click()
    cy.contains('操作成功')
    cy.get('@settings.all').should('have.length', 1)
    cy.tick(31000)
    cy.contains('button', '刷新后端设置').first().click()
    cy.get('@settings.all').should('have.length', 2)
  })

  for (const cache of ['fresh', 'expired', 'invalid', 'bad-shape', 'failed-envelope']) {
    it(`本地缓存${cache}按有效期与格式决定是否请求后端`, () => {
      serveSettings()
      cy.visit('/', { onBeforeLoad(win) {
        win.localStorage.setItem('rk_wcfr_web_prefs_v1', JSON.stringify({ cacheStrategy: 'local-5m', logLevel: 'debug' }))
        const value = cache === 'invalid' ? '{' : JSON.stringify({
          expiresAt: cache === 'bad-shape' ? 'invalid' : Date.now() + (cache === 'expired' ? -1 : 60000),
          json: cache === 'failed-envelope' ? { ok: false, error: { code: 'fixture', message: '旧缓存失败' } } : { ok: true, data: settingsFixture },
        })
        win.localStorage.setItem('rk_wcfr_cache:v1_settings', value)
      } })
      cy.contains('RK3288 控制台')
      cy.get('@settings.all').should('have.length', cache === 'fresh' ? 0 : 1)
    })
  }

  it('概览展示后端错误详情，并通过卡片重试恢复摘要', () => {
    cy.intercept('GET', '/api/v1/settings', { statusCode: 503, body: { ok: false, error: { code: 'fixture', message: '合成连接失败', details: ['合成配置尚未就绪'] } } })
    cy.visit('/')
    cy.contains('继续进入').click()
    cy.contains('li', '合成配置尚未就绪')
    serveSettings()
    cy.contains('.ant-alert', '后端 /api/v1/settings 不可用').contains('button', '重试').click()
    cy.contains('后端 /api/v1/settings 不可用').should('not.exist')
    cy.contains('.ant-card', '后端 settings（只读摘要）').contains('640×480')
  })

  for (const httpError of [false, true]) {
    it(`模型查询${httpError ? 'HTTP 错误' : '失败信封'}显示原因并允许重新拉取`, () => {
      serveSettings()
      cy.intercept('GET', '/api/v1/models', { statusCode: httpError ? 503 : 200, body: { ok: false, error: { code: 'fixture', message: '合成模型查询失败' } } })
      visitSettings()
      expandPanel('模型状态 (Model Status)')
      cy.contains('合成模型查询失败')
      cy.contains('button', '保存到后端').should('be.enabled')
    })
  }

  for (const scenario of ['models-error', 'empty-models', 'reload-error']) {
    it(`降级重试在 ${scenario} 时结束加载并显示原因`, () => {
      serveSettings()
      cy.intercept('GET', '/api/v1/health', { body: { ok: true, data: { status: 'degraded', availableFeatures: [] } } })
      cy.intercept('GET', '/api/v1/models', { body: scenario === 'models-error'
        ? { ok: false, error: { code: 'fixture', message: '合成列表拒绝' } }
        : { ok: true, data: { activeModels: scenario === 'empty-models' ? [] : [{ id: 'fixture' }] } } })
      cy.intercept('POST', '/api/v1/models/reload', { body: { ok: false, error: { code: 'fixture', message: '合成重载拒绝' } } })
      cy.visit('/')
      cy.contains('引擎处于降级运行模式')
      cy.contains('button', '重试').click()
      cy.contains(scenario === 'models-error' ? '合成列表拒绝' : scenario === 'empty-models' ? '当前没有可重载的模型' : '合成重载拒绝')
      cy.contains('button', '重试').should('be.enabled')
    })
  }

  it('健康探针失败时显示离线徽标，且不伪报模型降级', () => {
    serveSettings()
    cy.intercept('GET', '/api/v1/health', { statusCode: 503, body: '' }).as('offlineHealth')
    cy.visit('/')
    cy.wait('@offlineHealth')
    cy.get('.ant-badge-status-text').should('contain', '未连接')
    cy.contains('引擎处于降级运行模式').should('not.exist')
  })

  for (const timeout of [1, 999999]) {
    it(`本地超时配置 ${timeout} 被限制在合法范围内`, () => {
      cy.visit('/#/settings', { onBeforeLoad(win) {
        win.localStorage.setItem('rk_wcfr_web_prefs_v1', JSON.stringify({ apiTimeoutMs: timeout }))
      } })
      expandPanel('高级（API 超时 / 日志级别 / 缓存策略）')
      cy.get('#apiTimeoutMs').should('have.value', timeout === 1 ? '500' : '120000')
    })
  }

  it('本地偏好损坏时仍可进入设置并使用默认值', () => {
    cy.visit('/#/settings', { onBeforeLoad(win) {
      win.localStorage.setItem('rk_wcfr_web_prefs_v1', '{')
    } })
    cy.get('#theme').closest('.ant-select').should('contain', '跟随系统')
    cy.contains('button', '保存 Token').should('be.enabled')
  })

  it('浏览器拒绝读取本地存储时，设置页面与后端请求仍可用', () => {
    serveSettings()
    cy.visit('/#/settings', { onBeforeLoad(win) {
      cy.stub(win.Storage.prototype, 'getItem').throws(new Error('合成存储不可读'))
    } })
    cy.contains('button', '刷新后端设置').first().click()
    cy.wait('@settings').its('request.headers').should('not.have.property', 'authorization')
    cy.contains('操作成功')
    cy.contains('页面异常').should('not.exist')
  })

  it('响应引起渲染异常时展示错误边界，并能重新加载恢复', () => {
    serveSettings({ ...settingsFixture, schemaVersion: { malformed: true } })
    cy.on('uncaught:exception', (error) => {
      if (error.message.includes('Objects are not valid as a React child')) return false
    })
    cy.visit('/')
    cy.contains('页面异常')
    serveSettings()
    cy.contains('button', '重新加载').click()
    cy.contains('RK3288 控制台')
  })
})

