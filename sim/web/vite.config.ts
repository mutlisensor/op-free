import { defineConfig } from "vite";
import { catalogPlugin } from "./catalog.ts";

export default defineConfig({
  plugins: [catalogPlugin()],
  server: { port: 5174 },
  build: { target: "es2022" },
});
