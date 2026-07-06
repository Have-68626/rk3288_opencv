import { describe, it, expect, vi, beforeEach } from 'vitest';

const mockFetch = vi.fn();
globalThis.fetch = mockFetch;

describe('API Settings', () => {
  beforeEach(() => {
    mockFetch.mockReset();
  });

  it('should parse settings response correctly', async () => {
    const mockResponse = {
      ok: true,
      json: async () => ({
        schemaVersion: '1.0',
        camera: { deviceIndex: 0, resolution: '640x480' },
        inference: { throttleMode: 'CONTINUOUS', intervalMs: 500 },
      }),
    };
    mockFetch.mockResolvedValue(mockResponse);

    const response = await fetch('/api/v1/settings');
    const data = await response.json();

    expect(data.schemaVersion).toBe('1.0');
    expect(data.inference.throttleMode).toBe('CONTINUOUS');
    expect(data.camera.resolution).toBe('640x480');
  });

  it('should handle fetch error gracefully', async () => {
    mockFetch.mockRejectedValue(new Error('Network error'));
    await expect(fetch('/api/v1/settings')).rejects.toThrow('Network error');
  });
});
