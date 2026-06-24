// The `ws` package is an optional Node-only dependency, imported dynamically
// and used only through the minimal browser-WebSocket surface (SocketLike).
// We don't depend on its types; this keeps the build self-contained whether or
// not @types are installed.
declare module "ws";
