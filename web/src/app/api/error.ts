/** Safely extract a human-readable message from any catch clause value. */
export function getErrorMessage(e: unknown, fallback = '未知错误'): string {
  if (typeof e === 'string') return e
  if (typeof e === 'object' && e !== null && 'message' in e && typeof (e as Record<string, unknown>).message === 'string') {
    return (e as Record<string, unknown>).message as string
  }
  return fallback
}
