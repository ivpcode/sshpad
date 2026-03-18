import { defineConfig } from 'vite';

export default defineConfig({
  root: '.',
  build: {
    outDir: 'dist',
    emptyOutDir: true,
    // Genera file con nomi fissi (no hash) per il server C
    rollupOptions: {
      output: {
        entryFileNames: 'assets/[name].js',
        chunkFileNames: 'assets/[name].js',
        assetFileNames: 'assets/[name][extname]',
      },
    },
  },
  server: {
    // Dev server con proxy verso il backend C per le API
    proxy: {
      '/api': 'http://127.0.0.1:0', // porta da impostare a runtime
    },
  },
});
