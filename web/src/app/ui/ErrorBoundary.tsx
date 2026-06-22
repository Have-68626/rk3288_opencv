import { Button, Result } from 'antd'
import { Component, ErrorInfo, ReactNode } from 'react'

interface Props { children: ReactNode }
interface State { hasError: boolean; error: Error | null }

export class ErrorBoundary extends Component<Props, State> {
  state: State = { hasError: false, error: null }

  static getDerivedStateFromError(error: Error): State {
    return { hasError: true, error }
  }

  componentDidCatch(error: Error, info: ErrorInfo) {
    console.error('[ErrorBoundary]', error, info.componentStack)
  }

  render() {
    if (this.state.hasError) {
      return (
        <Result
          status="error"
          title="页面异常"
          subTitle={this.state.error?.message ?? '未知错误'}
          extra={
            <Button type="primary" onClick={() => { this.setState({ hasError: false, error: null }); window.location.reload() }}>
              重新加载
            </Button>
          }
        />
      )
    }
    return this.props.children
  }
}
