// 本文件仅验证浏览器 UI 的 mock 冒烟，不承担真实 Windows 后端联调。
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
    cy.get('#theme').click()
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
})

