import * as esbuild from "esbuild";
import { execSync } from "node:child_process";
import * as fs from "node:fs";

console.log("Building Orlyscript Playground...");

// 1. Compile backend server TypeScript
console.log("Compiling server TypeScript...");
execSync("npx tsc -p tsconfig.json", { stdio: "inherit" });

// 2. Bundle browser frontend with esbuild
console.log("Bundling frontend with esbuild...");
await esbuild.build({
  entryPoints: ["src/web/app.ts"],
  bundle: true,
  outfile: "public/bundle.js",
  format: "esm",
  sourcemap: true,
  minify: false,
  target: ["es2022"]
});

console.log("Build complete: public/bundle.js and dist/server.js generated successfully.");
