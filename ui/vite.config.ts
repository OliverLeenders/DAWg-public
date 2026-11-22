/// <reference types="vitest/config" />
import { defineConfig } from 'vite';
import react from '@vitejs/plugin-react';

// The built UI is served by the app's resource provider, so asset URLs must be relative.
export default defineConfig({
  base: './',
  plugins: [react()],
  server: { port: 5173, strictPort: true },
  build: { outDir: 'dist', emptyOutDir: true, target: 'es2022', sourcemap: true },
  // tokens.test.ts reads the tokens' source; Vitest empties other stylesheets.
  test: { css: { include: [/tokens\.css/] } },
});
