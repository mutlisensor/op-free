import { defineConfig } from "vite";
import { cataloguePlugin } from "./scripts/catalogue.ts";

// Relative base: the built site runs from any folder, including https://<user>.github.io/op-free/.
export default defineConfig({
  base: "./",
  plugins: [cataloguePlugin()],
  server: { port: 5180 },
  build: { target: "es2022" },
  worker: { format: "es" },
});
