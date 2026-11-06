import { defineConfig } from "vite"
import react from "@vitejs/plugin-react"
import tailwindcss from "@tailwindcss/vite"
import path from "node:path"

// Dev: `npm run dev` (port 5173) proxies the Flask studio API (studio/server.py on 127.0.0.1:8750).
// Build: `npm run build` writes dist/, which server.py serves at "/" (the old page stays at /classic).
export default defineConfig({
  plugins: [react(), tailwindcss()],
  resolve: {
    alias: {
      "@marquee/ui": path.resolve(__dirname, "../../packages/ui/src"),
      "@marquee/sign-player": path.resolve(__dirname, "../../packages/sign-player/src/index.ts"),
      "@": path.resolve(__dirname, "src"),
    },
  },
  server: {
    host: process.env.VITE_HOST || "127.0.0.1",   // 0.0.0.0 in the container
    port: 5173,
    proxy: {
      // The Flask studio API: the studio-api service in the container, localhost on the host.
      "/api": process.env.VITE_API_PROXY || "http://127.0.0.1:8750",
      "/emoji": process.env.VITE_API_PROXY || "http://127.0.0.1:8750",
    },
  },
  build: { outDir: "dist", emptyOutDir: true, chunkSizeWarningLimit: 900 },
})
