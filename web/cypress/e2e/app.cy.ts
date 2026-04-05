describe('RK3288 本地控制台（最小 E2E）', () => {
  it('可进入概览与设置，并保存本地偏好', () => {
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

    cy.contains('设置').click()
    cy.contains('基础（主题 / 语言 / 启动页）')
    cy.contains('保存本地偏好').click()
    cy.contains('保存成功')
  })

  it('后端 settings 请求失败时能展示错误提示', () => {
    cy.intercept('GET', '/api/v1/settings', {
      statusCode: 500,
      body: { ok: false, error: { code: 'internal_error', message: 'boom' } },
    }).as('getSettings')

    cy.visit('/#/home')
    cy.wait('@getSettings')
    cy.contains('后端 /api/v1/settings 不可用')
  })
})

