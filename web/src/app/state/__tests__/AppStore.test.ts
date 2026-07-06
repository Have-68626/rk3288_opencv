import { describe, it, expect } from 'vitest';

describe('AppState transitions', () => {
  it('should toggle between MOTION_TRIGGERED and CONTINUOUS', () => {
    const modes = ['MOTION_TRIGGERED', 'CONTINUOUS'] as const;
    let current: string = modes[0];
    const toggle = () => {
      current = current === modes[0] ? modes[1] : modes[0];
    };
    toggle();
    expect(current).toBe('CONTINUOUS');
    toggle();
    expect(current).toBe('MOTION_TRIGGERED');
  });
});
