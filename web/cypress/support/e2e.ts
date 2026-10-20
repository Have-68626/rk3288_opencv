import '@cypress/code-coverage/support'

beforeEach(function () {
  cy.task(
    'e2eProgress',
    { event: '用例开始', title: this.currentTest?.fullTitle() ?? '未知用例' },
    { log: false },
  )
})

afterEach(function () {
  const test = this.currentTest
  const state = test?.state === 'passed' ? '用例通过' : test?.state === 'failed' ? '用例失败' : '用例结束'
  cy.task('e2eProgress', { event: state, title: test?.fullTitle() ?? '未知用例' }, { log: false })
})

