# Playground service image: bundles frontend and runs Node.js session proxy
FROM node:20-alpine AS build

WORKDIR /app
COPY clients/ts clients/ts
COPY examples/playground examples/playground

WORKDIR /app/examples/playground
RUN npm install
RUN npm run build

# Runtime stage
FROM node:20-alpine

WORKDIR /app
COPY --from=build /app/clients/ts /app/clients/ts
COPY --from=build /app/examples/playground/node_modules /app/examples/playground/node_modules
COPY --from=build /app/examples/playground/package.json /app/examples/playground/package.json
COPY --from=build /app/examples/playground/dist /app/examples/playground/dist
COPY --from=build /app/examples/playground/public /app/examples/playground/public

WORKDIR /app/examples/playground
EXPOSE 3000

ENV PORT=3000
ENV HOST=0.0.0.0
CMD ["node", "dist/server.js"]
