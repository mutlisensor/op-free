import { defineConfig } from "vite";

// Relative base: the built site runs from any folder, including https://<user>.github.io/op-free/.
export default defineConfig({
  base: "./",
  server: { port: 5180 },
  build: { target: "es2022" },
});
